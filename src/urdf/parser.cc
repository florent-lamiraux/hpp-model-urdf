// Copyright (C) 2012 by Antonio El Khoury.
//
// This file is part of the hpp-model-urdf.
//
// hpp-model-urdf is free software: you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation, either version 3 of
// the License, or (at your option) any later version.
//
// hpp-model-urdf is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with hpp-model-urdf.  If not, see
// <http://www.gnu.org/licenses/>.

/**
 * \file src/urdf/parser.cc
 *
 * \brief Implementation of URDF Parser for hpp-model.
 */

#include <boost/filesystem/fstream.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>

#include <LinearMath/btMatrix3x3.h>
#include <LinearMath/btQuaternion.h>

#include <resource_retriever/retriever.h>

#include <KineoModel/kppSMLinearComponent.h>
#include <KineoModel/kppJointComponent.h>
#include <KineoModel/kppSolidComponentRef.h>
#include <KineoKCDModel/kppKCDPolyhedron.h>
#include <KineoKCDModel/kppKCDCylinder.h>
#include <KineoKCDModel/kppKCDBox.h>

#include <hpp/util/debug.hh>

#include <hpp/model/anchor-joint.hh>
#include <hpp/model/freeflyer-joint.hh>
#include <hpp/model/rotation-joint.hh>
#include <hpp/model/translation-joint.hh>

#include <hpp/geometry/component/capsule.hh>

#include <hpp/model/urdf/parser.hh>
#include <hpp/model/urdf/util.hh>

namespace hpp
{
  namespace model
  {
    namespace urdf
    {
      Parser::Parser ()
	: model_ (),
	  robot_ (),
	  rootJoint_ (),
	  jointsMap_ (),
	  factory_ (),
	  waistJointName_ (),
	  chestJointName_ (),
	  leftWristJointName_ (),
	  rightWristJointName_ (),
	  leftHandJointName_ (),
	  rightHandJointName_ (),
	  leftAnkleJointName_ (),
	  rightAnkleJointName_ (),
	  leftFootJointName_ (),
	  rightFootJointName_ (),
	  gazeJointName_ ()
      {}

      Parser::~Parser ()
      {}

      void
      Parser::displayFoot (CjrlFoot *aFoot, std::ostream &os)
      {
      	vector3d data;

      	aFoot->getAnklePositionInLocalFrame (data);
      	os << "Ankle position in local frame: " << data << std::endl;

      	double lFootWidth=0.0, lFootDepth=0.0;
      	aFoot->getSoleSize (lFootDepth, lFootWidth);
      	os << "Foot width: " << lFootWidth
      	   << " foot depth: " << lFootDepth << std::endl;
      }

      void
      Parser::displayHand (CjrlHand *aHand, std::ostream &os)
      {
      	vector3d data;

      	aHand->getCenter (data);
      	os << "Center: " << data << std::endl;

      	aHand->getThumbAxis (data);
      	os << "Thumb axis: " << data << std::endl;

      	aHand->getForeFingerAxis (data);
      	os << "Showing axis: " << data << std::endl;

      	aHand->getPalmNormal (data);
      	os << "Palm axis: " << data << std::endl;
      }

      void
      Parser::displayEndEffectors (std::ostream &os)
      {
      	CjrlHand *aHand;
      	aHand = robot_->leftHand ();
      	displayHand(aHand,os);
      	aHand = robot_->rightHand ();
      	displayHand (aHand, os);

      	CjrlFoot *aFoot;
      	aFoot = robot_->leftFoot ();
      	displayFoot (aFoot, os);
      	aFoot = robot_->rightFoot ();
      	displayFoot (aFoot, os);
      }

      void Parser::displayActuatedJoints (std::ostream &os)
      {
      	const vectorN currentConfiguration = robot_->currentConfiguration ();
      	os << "Actuated joints : " ;
	std::vector<CjrlJoint*> actJointsVect = actuatedJoints ();
	for (unsigned int i = 0; i < actJointsVect.size (); i++)
      	  {
      	    unsigned int riC = actJointsVect[i]->rankInConfiguration ();
      	    os << currentConfiguration[riC] << " ";
      	  }
      	os << std::endl;
      }

      namespace
      {
	/// \brief Convert joint orientation to standard
	/// jrl-dynamics accepted orientation.
	///
	/// abstract-robot-dynamics do not contain any information
	/// about around which axis a rotation joint rotates.
	/// On the opposite, it makes the assumption it is around the X
	/// axis. We have to make sure this is the case here.
	///
	/// We use Gram-Schmidt process to compute the rotation matrix.
	///
	/// [1] http://en.wikipedia.org/wiki/Gram%E2%80%93Schmidt_process
	CkitMat4
	normalizeFrameOrientation (Parser::UrdfJointConstPtrType urdfJoint)
	{
	  if (!urdfJoint)
	    {
	      hppDout (error, "Null pointer in normalizeFrameOrientation");
	      CkitMat4 result;
	      result.identity ();
	      return result;
	    }

	  CkitMat4 result;
	  result.identity ();

	  vector3d x (urdfJoint->axis.x,
		      urdfJoint->axis.y,
		      urdfJoint->axis.z);
	  x.normalize ();

	  vector3d y (0., 0., 0.);
	  vector3d z (0., 0., 0.);

	  unsigned smallestComponent = 0;
	  for (unsigned i = 0; i < 3; ++i)
	    if (std::fabs(x[i]) < std::fabs(x[smallestComponent]))
	      smallestComponent = i;

	  y[smallestComponent] = 1.;
	  z = x ^ y;
	  y = z ^ x;
	  // (x, y, z) is an orthonormal basis.

	  for (unsigned i = 0; i < 3; ++i)
	    {
	      result (i, 0) = x[i];
	      result (i, 1) = y[i];
	      result (i, 2) = z[i];
	    }

	  return result;
	}
      } // end of anonymous namespace.

      void
      Parser::findSpecialJoint (const std::string& repName,
				std::string& jointName)
      {
	UrdfLinkPtrType linkPtr = model_.links_[repName];
	if (linkPtr)
	  {
	    UrdfJointPtrType joint = linkPtr->parent_joint;
	    if (joint)
	      jointName = joint->name;
	  }
      }

      void
      Parser::findSpecialJoints ()
      {
	waistJointName_ = "base_joint";
	findSpecialJoint ("torso", chestJointName_);
	findSpecialJoint ("l_wrist", leftWristJointName_);
	findSpecialJoint ("r_wrist", rightWristJointName_);
	findSpecialJoint ("l_gripper", leftHandJointName_);
	findSpecialJoint ("r_gripper", rightHandJointName_);
	findSpecialJoint ("l_ankle", leftAnkleJointName_);
	findSpecialJoint ("r_ankle", rightAnkleJointName_);
	findSpecialJoint ("l_sole", leftFootJointName_);
	findSpecialJoint ("r_sole", rightFootJointName_);
	findSpecialJoint ("gaze", gazeJointName_);
	//FIXME: we are missing toes in abstract-robot-dynamics for now.
      }

      void
      Parser::setSpecialJoints ()
      {
	if (!findJoint (waistJointName_))
	  hppDout (notice, "No waist joint found");
	else
	  robot_->waist (findJoint (waistJointName_)->jrlJoint ());
	if (!findJoint (chestJointName_))
	  hppDout (notice, "No chest joint found");
	else
	  robot_->chest (findJoint (chestJointName_)->jrlJoint ());
	if (!findJoint (leftWristJointName_))
	  hppDout (notice, "No left wrist joint found");
	else
	  robot_->leftWrist (findJoint (leftWristJointName_)->jrlJoint ());
	if (!findJoint (rightWristJointName_))
	  hppDout (notice, "No right wrist joint found");
	else
	  robot_->rightWrist (findJoint (rightWristJointName_)->jrlJoint ());
	if (!findJoint (leftAnkleJointName_))
	  hppDout (notice, "No left ankle joint found");
	else
	  robot_->leftAnkle (findJoint (leftAnkleJointName_)->jrlJoint ());
	if (!findJoint (rightAnkleJointName_))
	  hppDout (notice, "No right ankle joint found");
	else
	  robot_->rightAnkle (findJoint (rightAnkleJointName_)->jrlJoint ());
	if (!findJoint (gazeJointName_))
	  hppDout (notice, "No gaze joint found");
	else
	  robot_->gazeJoint (findJoint (gazeJointName_)->jrlJoint ());
      }

      bool
      Parser::parseJoints ()
      {
	// Create free floating joint.
	// FIXME: position set to identity for now.
	CkitMat4 position;
	position.identity ();
	rootJoint_ = createFreeflyerJoint ("base_joint", position);
	if (!rootJoint_)
	  {
	    hppDout (error, "Failed to create root joint (free flyer)");
	    return false;
	  }
	
	robot_->setRootJoint(rootJoint_);

	// Iterate through each "true cinematic" joint and create a
	// corresponding CjrlJoint.
	for(MapJointType::const_iterator it = model_.joints_.begin();
	    it != model_.joints_.end(); ++it)
	  {
	    position =
	      getPoseInReferenceFrame("base_footprint_joint", it->first);

	    // Normalize orientation if this is an actuated joint.
	    UrdfJointConstPtrType joint = model_.getJoint (it->first);
	    if (joint->type == ::urdf::Joint::REVOLUTE
		|| joint->type == ::urdf::Joint::CONTINUOUS
		|| joint->type == ::urdf::Joint::PRISMATIC)
	      position = position * normalizeFrameOrientation (joint);

	    switch(it->second->type)
	      {
	      case ::urdf::Joint::UNKNOWN:
		hppDout (error, "Parsed joint has UNKNOWN type,"
			 << " this should not happen");
		return false;
		break;
	      case ::urdf::Joint::REVOLUTE:
		createRotationJoint (it->first, position, it->second->limits);
		break;
	      case ::urdf::Joint::CONTINUOUS:
		createContinuousJoint (it->first, position);
		break;
	      case ::urdf::Joint::PRISMATIC:
		createTranslationJoint (it->first, position,
					it->second->limits);
		break;
	      case ::urdf::Joint::FLOATING:
		createFreeflyerJoint (it->first, position);
		break;
	      case ::urdf::Joint::PLANAR:
		hppDout (error, "PLANAR joints are not supported");
		return false;
		break;
	      case ::urdf::Joint::FIXED:
		createAnchorJoint (it->first, position);
		break;
	      default:
		hppDout (error, "Unknown joint type " << (int)it->second->type
			 << ": should never happen");
		return false;
	      }
	  }
	
	return true;
      }

      std::vector<CjrlJoint*> Parser::actuatedJoints ()
      {
	std::vector<CjrlJoint*> jointsVect;

	typedef std::map<std::string, boost::shared_ptr< ::urdf::Joint > >
	  jointMap_t;

        for(jointMap_t::const_iterator it = model_.joints_.begin ();
	    it != model_.joints_.end (); ++it)
	  {
	    if (!it->second)
	      throw std::runtime_error ("null joint shared pointer");
	    if (it->second->type == ::urdf::Joint::UNKNOWN
		|| it->second->type == ::urdf::Joint::FLOATING
		|| it->second->type == ::urdf::Joint::FIXED)
	      continue;
	    MapHppJointType::const_iterator child = jointsMap_.find (it->first);
	    if (child == jointsMap_.end () || !child->second)
	      throw std::runtime_error ("failed to compute actuated joints");

	    // The joints already exists in the vector, do not add it twice.
	    if (std::find (jointsVect.begin (),
			   jointsVect.end (),
			   child->second->jrlJoint ()) != jointsVect.end ())
	      continue;
	    jointsVect.push_back (child->second->jrlJoint ());
	  }
	return jointsVect;
      }

      bool
      Parser::connectJoints (const Parser::JointPtrType& rootJoint)
      {
	BOOST_FOREACH (const std::string& childName,
		       getChildrenJoint (rootJoint->kppJoint ()->name ()))
	  {
	    MapHppJointType::const_iterator child = jointsMap_.find (childName);
	    if (child == jointsMap_.end () && !!child->second)
	      {
		hppDout (error, "Failed to connect joint " << childName);
		return false;
	      }
	    rootJoint->addChildJoint (child->second);
	    connectJoints (child->second);
	  }

	return true;
      }

      bool
      Parser::addBodiesToJoints ()
      {
        for(MapHppJointType::const_iterator it = jointsMap_.begin();
	    it != jointsMap_.end(); ++it)
	  {
	    // Retrieve associated URDF joint.
	    UrdfJointConstPtrType joint = model_.getJoint (it->first);
	    if (!joint && it->first != "base_joint")
	      continue;

	    // Retrieve joint name.
	    std::string childLinkName;
	    if (it->first == "base_joint")
	      childLinkName = "base_link";
	    else
	      childLinkName = joint->child_link_name;

	    // Get child link.
	    UrdfLinkConstPtrType link = model_.getLink (childLinkName);
	    if (!link)
	      {
		hppDout (error, "Link " << childLinkName
			 << " not found, inconsistent model");
		return false;
	      }

	    // Retrieve inertial information.
	    boost::shared_ptr< ::urdf::Inertial> inertial =
	      link->inertial;

	    vector3d localCom (0., 0., 0.);
	    matrix3d inertiaMatrix;
	    double mass = 0.;
	    if (inertial)
	      {
		localCom[0] = inertial->origin.position.x;
		localCom[1] = inertial->origin.position.y;
		localCom[2] = inertial->origin.position.z;

		mass = inertial->mass;

		inertiaMatrix (0, 0) = inertial->ixx;
		inertiaMatrix (0, 1) = inertial->ixy;
		inertiaMatrix (0, 2) = inertial->ixz;

		inertiaMatrix (1, 0) = inertial->ixy;
		inertiaMatrix (1, 1) = inertial->iyy;
		inertiaMatrix (1, 2) = inertial->iyz;

		inertiaMatrix (2, 0) = inertial->ixz;
		inertiaMatrix (2, 1) = inertial->iyz;
		inertiaMatrix (2, 2) = inertial->izz;

		// Use joint normalization to properly reorient
		// inertial frames.
		if (it->first == "base_joint")
		  {}
		else
		  if (link->parent_joint->type == ::urdf::Joint::REVOLUTE
		      || link->parent_joint->type == ::urdf::Joint::CONTINUOUS
		      || link->parent_joint->type == ::urdf::Joint::PRISMATIC)
		  {
		    CkitMat4 normalizedJointTransform
		      = normalizeFrameOrientation (link->parent_joint);

		    CkitMat4 localComTransform;
		    localComTransform.identity ();
		    localComTransform(0, 3) = localCom[0];
		    localComTransform(1, 3) = localCom[1];
		    localComTransform(2, 3) = localCom[2];
		    localComTransform = normalizedJointTransform.inverse ()
		      * localComTransform;
		    localCom[0] = localComTransform(0,3);
		    localCom[1] = localComTransform(1,3);
		    localCom[2] = localComTransform(2,3);

		    CkitMat4 inertiaMatrixTransform;
		    inertiaMatrixTransform.identity ();
		    for (unsigned i = 0; i < 3; ++i)
		      for (unsigned j = 0; j < 3; ++j)
			inertiaMatrixTransform(i, j) = inertiaMatrix(i, j);
		    inertiaMatrixTransform
		      = normalizedJointTransform.inverse ()
		      * inertiaMatrixTransform
		      * normalizedJointTransform;
		    for (unsigned i = 0; i < 3; ++i)
		      for (unsigned j = 0; j < 3; ++j)
			inertiaMatrix(i, j) = inertiaMatrixTransform(i, j);
		  }
	      }
	    else
	      hppDout (notice, "missing inertial information in link "
		       << childLinkName);

	    // Create dynamic body and fill inertial information.
	    CjrlBody* jrlBody = factory_.createBody ();
	    jrlBody->mass (mass);
	    jrlBody->localCenterOfMass (localCom);
	    jrlBody->inertiaMatrix (inertiaMatrix);

	    // Link dynamic body to dynamic joint.
	    it->second->jrlJoint ()->setLinkedBody (*jrlBody);

	    // Link geometric body to joint.
	    // BodyPtrType body = hpp::model::Body::create (childLinkName);
	    // std::cout << childLinkName << " " << body->innerObjects ().size ()
	    // 	      << std::endl;
	    // KIT_DYNAMIC_PTR_CAST(CkwsJoint, it->second)
	    //   ->setAttachedBody (body);

	    // Create geometric body and fill geometry information.
	    if (link->visual && link->collision)
	      {
		JointPtrType hppJoint
		  = KIT_DYNAMIC_PTR_CAST (JointType, it->second);
		if (!addSolidComponentToJoint (link, hppJoint))
		  {
		    hppDout (error, "Could not add solid component to joint.");
		    return false;
		  }

		BodyPtrType body
		  = KIT_DYNAMIC_PTR_CAST (BodyType,
					  hppJoint->kppJoint ()->kwsKCDBody ());
		body->name (childLinkName);
	      }
	  }

	return true;
      }

      CkitMat4
      Parser::computeBodyAbsolutePosition
      (const Parser::UrdfLinkConstPtrType& link, const ::urdf::Pose& pose)
      {
	CkitMat4 linkPositionInParentJoint = poseToMatrix (pose);

	CkitMat4 parentJointInWorld;
	if (link->name == "base_link")
	  parentJointInWorld = findJoint ("base_joint")->kppJoint ()
	    ->kwsJoint ()->currentPosition ();
	else
	  parentJointInWorld = findJoint (link->parent_joint->name)->kppJoint ()
	    ->kwsJoint ()->currentPosition ();

	// Denormalize orientation if this is an actuated joint.
	if (link->name == "base_link")
	  {}
	else
	if (link->parent_joint->type == ::urdf::Joint::REVOLUTE
	    || link->parent_joint->type == ::urdf::Joint::CONTINUOUS
	    || link->parent_joint->type == ::urdf::Joint::PRISMATIC)
	  parentJointInWorld = parentJointInWorld
	    * normalizeFrameOrientation (link->parent_joint).inverse ();

	CkitMat4 position = parentJointInWorld * linkPositionInParentJoint;
	return position;
      }

      bool
      Parser::addSolidComponentToJoint (const UrdfLinkConstPtrType& link,
					const JointPtrType& joint)
      {
	boost::shared_ptr< ::urdf::Visual> visual =
	  link->visual;
	boost::shared_ptr< ::urdf::Collision> collision =
	  link->collision;

	// Handle the case where visual geometry is a mesh and
	// collision geometry is a mesh.
	if (visual->geometry->type == ::urdf::Geometry::MESH
	    && collision->geometry->type == ::urdf::Geometry::MESH)
	  {
	    boost::shared_ptr< ::urdf::Mesh> visualGeometry
	      = dynamic_pointer_cast< ::urdf::Mesh> (visual->geometry);
	    boost::shared_ptr< ::urdf::Mesh> collisionGeometry
	      = dynamic_pointer_cast< ::urdf::Mesh> (collision->geometry);
	    std::string visualFilename = visualGeometry->filename;
	    std::string collisionFilename = collisionGeometry->filename;
	    ::urdf::Vector3 scale = visualGeometry->scale;

	    // FIXME: We assume for now that visual and collision
	    // meshes are the same.
	    if (visualFilename != collisionFilename)
	      {
		hppDout (error,
			 "Unhandled:visual and collision meshes not the same for "
			 << link->name);
		return false;
	      }

	    // Create Kite polyhedron component by parsing Collada
	    // file.
	    CkppKCDPolyhedronShPtr polyhedron
	      = CkppKCDPolyhedron::create (link->name);
	    if (!loadPolyhedronFromResource (visualFilename, scale, polyhedron))
	      {
		hppDout (error,
			 "Could not load polyhedron from resource for "
			 << link->name);
		return false;
	      }
	    polyhedron->makeCollisionEntity (CkcdObject::IMMEDIATE_BUILD);

	    // Compute body position in world frame.
	    CkitMat4 position = computeBodyAbsolutePosition (link,
							     visual->origin);
	    polyhedron->setAbsolutePosition (position);

	    // Add solid component.
	    joint->kppJoint ()->addSolidComponentRef
	      (CkppSolidComponentRef::create (polyhedron));
	  }

	// Handle the case where visual geometry is a cylinder and
	// collision geometry is a cylinder.
	if (visual->geometry->type == ::urdf::Geometry::CYLINDER
	    && collision->geometry->type == ::urdf::Geometry::CYLINDER)
	  {
	    boost::shared_ptr< ::urdf::Cylinder> visualGeometry
	      = dynamic_pointer_cast< ::urdf::Cylinder> (visual->geometry);
	    boost::shared_ptr< ::urdf::Cylinder> collisionGeometry
	      = dynamic_pointer_cast< ::urdf::Cylinder> (collision->geometry);

	    // FIXME: check whether visual and collision cylinder are the same.
	    double length = visualGeometry->length;
	    double radius = visualGeometry->radius;

	    // Create Kite cylinder component.
	    CkppKCDCylinderShPtr cylinder
	      = CkppKCDCylinder::create (link->name, radius, radius, length,
					 32, true, true);
	    cylinder->makeCollisionEntity (CkcdObject::IMMEDIATE_BUILD);

	    // Compute body position in world frame.
	    CkitMat4 position = computeBodyAbsolutePosition (link,
							     visual->origin);

	    // Apply additional transformation as cylinders in Kite
	    // are oriented along the x axis, while cylinders in urdf
	    // are oriented along the z axis.
	    CkitMat4 zTox;
	    zTox.rotateY (M_PI / 2);
	    position = position * zTox;
	    cylinder->setAbsolutePosition (position);

	    // Add solid component.
	    joint->kppJoint ()->addSolidComponentRef
	      (CkppSolidComponentRef::create (cylinder));
	  }

	// Handle the case where visual geometry is a box and
	// collision geometry is a box.
	if (visual->geometry->type == ::urdf::Geometry::BOX
	    && collision->geometry->type == ::urdf::Geometry::BOX)
	  {
	    boost::shared_ptr< ::urdf::Box> visualGeometry
	      = dynamic_pointer_cast< ::urdf::Box> (visual->geometry);
	    boost::shared_ptr< ::urdf::Box> collisionGeometry
	      = dynamic_pointer_cast< ::urdf::Box> (collision->geometry);

	    // FIXME: check whether visual and collision boxes are the same.
	    double x = visualGeometry->dim.x;
	    double y = visualGeometry->dim.y;
	    double z = visualGeometry->dim.z;

	    // Create Kite box component.
	    CkppKCDBoxShPtr box
	      = CkppKCDBox::create (link->name, x, y, z);
	    box->makeCollisionEntity (CkcdObject::IMMEDIATE_BUILD);

	    // Compute body position in world frame.
	    CkitMat4 position = computeBodyAbsolutePosition (link,
							     visual->origin);
	    box->setAbsolutePosition (position);

	    // Add solid component.
	    joint->kppJoint ()->addSolidComponentRef
	      (CkppSolidComponentRef::create (box));
	  }

	// Handle the case where visual geometry is a mesh and
	// collision geometry is a cylinder. In this case the
	// collision geometry KiteLab is considered to be a capsule.
	if (visual->geometry->type == ::urdf::Geometry::MESH
	    && collision->geometry->type == ::urdf::Geometry::CYLINDER)
	  {
	    boost::shared_ptr< ::urdf::Mesh> visualGeometry
	      = dynamic_pointer_cast< ::urdf::Mesh> (visual->geometry);
	    boost::shared_ptr< ::urdf::Cylinder> collisionGeometry
	      = dynamic_pointer_cast< ::urdf::Cylinder> (collision->geometry);

	    double length = collisionGeometry->length;
	    double radius = collisionGeometry->radius;

	    // Create capsule component.
	    using namespace hpp::geometry::component;
	    CapsuleShPtr capsule
	      = Capsule::create (link->name, length, radius);
	    capsule->makeCollisionEntity (CkcdObject::IMMEDIATE_BUILD);

	    // Compute body position in world frame.
	    CkitMat4 position = computeBodyAbsolutePosition (link,
							     collision->origin);

	    // Apply additional transformation as capsules
	    // are oriented along the x axis, while cylinders in urdf
	    // are oriented along the z axis.
	    CkitMat4 zTox;
	    zTox.rotateY (M_PI / 2);
	    position = position * zTox;
	    capsule->setAbsolutePosition (position);

	    // Create a segment that is equivalent to the
	    // capsule. This segment can be used for fast distance
	    // computation between two robot bodies.
	    CkcdPoint endPoint1, endPoint2;
	    kcdReal segmentRadius;
	    capsule->getCapsule (0, endPoint1, endPoint2,
				 segmentRadius);
	    std::string segmentName = capsule->name ();
	    segmentName.append ("-segment");
	    SegmentShPtr segment
	      = Segment::create (segmentName,
				     endPoint1,
				     endPoint2,
				     radius);
	    segment->makeCollisionEntity
	      (CkcdObject::IMMEDIATE_BUILD);
	    segment->setAbsolutePosition (position);

	    // Add solid component.
	    joint->kppJoint ()->addSolidComponentRef
	      (CkppSolidComponentRef::create (capsule));
	    joint->kppJoint ()->addSolidComponentRef
	      (CkppSolidComponentRef::create (segment));
	  }

	return true;
      }

      void
      Parser::setFreeFlyerBounds ()
      {
      	CjrlJoint * jrlRootJoint = robot_->getRootJoint ()->jrlJoint ();
      	hpp::model::JointShPtr hppRootJoint = robot_->getRootJoint ();

      	/* Translations */
      	for(unsigned int i = 0; i < 3; i++) {
      	  jrlRootJoint->lowerBound (i,
      				    - std::numeric_limits<double>::infinity ());
      	  jrlRootJoint->upperBound (i,
      				    std::numeric_limits<double>::infinity ());
      	}
      	/* Rx, Ry */
      	for(unsigned int i = 3; i < 5; i++){
      	  hppRootJoint->isBounded (i, true);
      	  hppRootJoint->lowerBound (i, -M_PI/6);
      	  hppRootJoint->upperBound (i, M_PI/6);
      	}
      	/* Rz */
      	jrlRootJoint->lowerBound (5, -std::numeric_limits<double>::infinity ());
      	jrlRootJoint->upperBound (5, std::numeric_limits<double>::infinity ());
      }

      namespace
      {
	vector3d
	vector4dTo3d (vector4d v)
	{
	  return vector3d (v[0], v[1], v[2]);
	}

	void
	matrix4dToRT (const matrix4d M, matrix3d& R, vector3d& v)
	{
	  for (unsigned i = 0; i < 3; ++i)
	    {
	      for (unsigned j = 0; j < 3; ++j)
		R(i, j) = M(i, j);
	      v[i] = M(i, 3);
	    }
	}

	void
	matrix3dToColumns (const matrix3d R,
			   vector3d& v0, vector3d& v1, vector3d& v2)
	{
	  for (unsigned i = 0; i < 3; ++i)
	    {
	      v0[i] = R(i, 0);
	      v1[i] = R(i, 1);
	      v2[i] = R(i, 2);
	    }
	}

      } // end of anonymous namespace.

      void
      Parser::computeHandsInformation
      (MapHppJointType::const_iterator& hand,
       MapHppJointType::const_iterator& wrist,
       vector3d& center,
       vector3d& thumbAxis,
       vector3d& foreFingerAxis,
       vector3d& palmNormal) const
      {
	matrix4d world_M_hand =
	  hand->second->jrlJoint ()->initialPosition ();
	matrix4d world_M_wrist =
	  wrist->second->jrlJoint ()->initialPosition ();
	matrix4d wrist_M_world;
	world_M_wrist.Inversion (wrist_M_world);

	// Hand to wrist transformation allows defining hand local
	// center and axes.
	matrix4d wrist_M_hand = wrist_M_world * world_M_hand;
	matrix3d wrist_R_hand;
	matrix4dToRT (wrist_M_hand, wrist_R_hand, center);
	matrix3dToColumns (wrist_R_hand, thumbAxis, foreFingerAxis, palmNormal);
      }

      void
      Parser::fillGaze ()
      {
	MapHppJointType::const_iterator gaze =
	  jointsMap_.find (gazeJointName_);
	CjrlJoint* gazeJoint = gaze->second->jrlJoint ();
	robot_->gazeJoint (gazeJoint);
	vector3d dir, origin;
	// Gaze direction is defined by the gaze joint local
	// orientation.
	dir[0] = 1;
	dir[1] = 0;
	dir[2] = 0;
	// Gaze position should is defined by the gaze joint local
	// origin.
	origin[0] = 0;
	origin[1] = 0;
	origin[2] = 0;
	robot_->gaze (dir, origin);
      }

      void
      Parser::fillHandsAndFeet ()
      {
	MapHppJointType::const_iterator leftHand =
	  jointsMap_.find (leftHandJointName_);
	MapHppJointType::const_iterator rightHand =
	  jointsMap_.find (rightHandJointName_);
	MapHppJointType::const_iterator leftWrist =
	  jointsMap_.find (leftWristJointName_);
	MapHppJointType::const_iterator rightWrist =
	  jointsMap_.find (rightWristJointName_);

	MapHppJointType::const_iterator leftFoot =
	  jointsMap_.find (leftFootJointName_);
	MapHppJointType::const_iterator rightFoot =
	  jointsMap_.find (rightFootJointName_);
	MapHppJointType::const_iterator leftAnkle =
	  jointsMap_.find (leftAnkleJointName_);
	MapHppJointType::const_iterator rightAnkle =
	  jointsMap_.find (rightAnkleJointName_);

	if (leftHand != jointsMap_.end () && leftWrist != jointsMap_.end ())
	  {
	    HandPtrType hand
	      = factory_.createHand (leftWrist->second->jrlJoint ());
	    hand->setAssociatedWrist(leftWrist->second->jrlJoint ());

	    vector3d center (0., 0., 0.);
	    vector3d thumbAxis (0., 0., 0.);
	    vector3d foreFingerAxis (0., 0., 0.);
	    vector3d palmNormal (0., 0., 0.);

	    computeHandsInformation
	      (leftHand, leftWrist,
	       center, thumbAxis, foreFingerAxis, palmNormal);

	    hand->setCenter (center);
	    hand->setThumbAxis (thumbAxis);
	    hand->setForeFingerAxis (foreFingerAxis);
	    hand->setPalmNormal (palmNormal);
	    robot_->leftHand (hand);
	  }
	else
	  hppDout (notice, "Could not set left hand");

	if (rightHand != jointsMap_.end () && rightWrist != jointsMap_.end ())
	  {
	    HandPtrType hand
	      = factory_.createHand (rightWrist->second->jrlJoint ());
	    hand->setAssociatedWrist(rightWrist->second->jrlJoint ());

	    vector3d center (0., 0., 0.);
	    vector3d thumbAxis (0., 0., 0.);
	    vector3d foreFingerAxis (0., 0., 0.);
	    vector3d palmNormal (0., 0., 0.);

	    computeHandsInformation
	      (rightHand, rightWrist,
	       center, thumbAxis, foreFingerAxis, palmNormal);

	    hand->setCenter (center);
	    hand->setThumbAxis (thumbAxis);
	    hand->setForeFingerAxis (foreFingerAxis);
	    hand->setPalmNormal (palmNormal);

	    robot_->rightHand (hand);
	  }
	else
	  hppDout (notice, "Could not set right hand");

	if (leftFoot != jointsMap_.end () && leftAnkle != jointsMap_.end ())
	  {
	    FootPtrType foot
	      = factory_.createFoot (leftAnkle->second->jrlJoint ());
	    foot->setAnklePositionInLocalFrame
	      (computeAnklePositionInLocalFrame (leftFoot, leftAnkle));

	    //FIXME: to be determined using robot contact points definition.
	    foot->setSoleSize (0., 0.);

	    robot_->leftFoot (foot);
	  }
	else
	  hppDout (notice, "Could not set left foot");

	if (rightFoot != jointsMap_.end () && rightAnkle != jointsMap_.end ())
	  {
	    FootPtrType foot
	      = factory_.createFoot (rightAnkle->second->jrlJoint ());
	    foot->setAnklePositionInLocalFrame
	      (computeAnklePositionInLocalFrame (rightFoot, rightAnkle));

	    //FIXME: to be determined using robot contact points definition.
	    foot->setSoleSize (0., 0.);

	    robot_->rightFoot (foot);
	  }
	else
	  hppDout (notice, "Could not set right foot");
      }

      std::vector<std::string>
      Parser::getChildrenJoint (const std::string& jointName)
      {
	std::vector<std::string> result;
	getChildrenJoint (jointName, result);
	return result;
      }

      bool
      Parser::getChildrenJoint (const std::string& jointName,
				std::vector<std::string>& result)
      {
	typedef boost::shared_ptr< ::urdf::Joint> jointPtr_t;

	boost::shared_ptr<const ::urdf::Joint> joint =
	  model_.getJoint (jointName);

	if (!joint && jointName != "base_joint")
	  {
	    hppDout (error, "Failed to retrieve children joints of joint "
		     << jointName);
	    return false;
	  }

	boost::shared_ptr<const ::urdf::Link> childLink;
	if (jointName == "base_joint")
	  childLink = model_.getLink ("base_link");
	else
	  childLink = model_.getLink (joint->child_link_name);

	if (!childLink)
	  {
	    hppDout (error, "Failed to retrieve children link of joint "
		     << jointName);
	  }

       	const std::vector<jointPtr_t>& jointChildren =
	  childLink->child_joints;

	BOOST_FOREACH (const jointPtr_t& joint, jointChildren)
	  {
	    if (jointsMap_.count(joint->name) > 0)
	      result.push_back (joint->name);
	    else
	      if (!getChildrenJoint (joint->name, result))
		{
		  hppDout (error, "Could not add children joint to joint "
			   << joint->name);
		  return false;
		}
	  }

	return true;
      }

      Parser::JointPtrType
      Parser::createFreeflyerJoint (const std::string& name,
				    const CkitMat4& mat)
      {
	JointPtrType joint;
	if (jointsMap_.find (name) != jointsMap_.end ())
	  {
	    hppDout (error, "Duplicated free flyer joint "
		     << name);
	    joint.reset ();
	    return joint;
	  }

      	joint = hpp::model::FreeflyerJoint::create (name, mat);
	for (unsigned i = 0; i < 6; ++i)
	  joint->isBounded (i, false);
      	jointsMap_[name] = joint;
	return joint;
      }

      Parser::JointPtrType
      Parser::createRotationJoint (const std::string& name,
				   const CkitMat4& mat,
				   const Parser::UrdfJointLimitsPtrType& limits)
      {
	JointPtrType joint;
	if (jointsMap_.find (name) != jointsMap_.end ())
	  {
	    hppDout (error, "Duplicated rotation joint "
		     << name);
	    joint.reset ();
	    return joint;
	  }

	joint = hpp::model::RotationJoint::create (name, mat);
	if (limits)
	  {
	    joint->isBounded (0, true);
	    joint->bounds (0, limits->lower, limits->upper);
	    joint->velocityBounds (0, -limits->velocity, limits->velocity);
	    joint->torqueBounds (0, -limits->effort, limits->effort);
	  }
      	jointsMap_[name] = joint;
	return joint;
      }

      Parser::JointPtrType
      Parser::createContinuousJoint (const std::string& name,
				     const CkitMat4& mat)
      {
	JointPtrType joint;
	if (jointsMap_.find (name) != jointsMap_.end ())
	  {
	    hppDout (error, "Duplicated continuous joint "
		     << name);
	    joint.reset ();
	    return joint;
	  }

      	joint = hpp::model::RotationJoint::create (name, mat);
	joint->isBounded (0, false);
      	jointsMap_[name] = joint;
	return joint;
      }

      Parser::JointPtrType
      Parser::createTranslationJoint (const std::string& name,
				      const CkitMat4& mat,
				      const Parser::
				      UrdfJointLimitsPtrType& limits)
      {
	JointPtrType joint;
	if (jointsMap_.find (name) != jointsMap_.end ())
	  {
	    hppDout (error, "Duplicated translation joint "
		     << name);
	    joint.reset ();
	    return joint;
	  }

      	joint = hpp::model::TranslationJoint::create (name, mat);
	if (limits)
	  {
	    joint->isBounded (0, true);
	    joint->bounds (0, limits->lower, limits->upper);
	    joint->velocityBounds (0, -limits->velocity, limits->velocity);
	    joint->torqueBounds (0, -limits->effort, limits->effort);
	  }
      	jointsMap_[name] = joint;
	return joint;
      }

      Parser::JointPtrType
      Parser::createAnchorJoint (const std::string& name, const CkitMat4& mat)
      {
	JointPtrType joint;
	if (jointsMap_.find (name) != jointsMap_.end ())
	  {
	    hppDout (error, "Duplicated anchor joint "
		     << name);
	    joint.reset ();
	    return joint;
	  }

      	joint = hpp::model::AnchorJoint::create (name, mat);
      	jointsMap_[name] = joint;
	return joint;
      }

      Parser::JointPtrType
      Parser::findJoint (const std::string& jointName)
      {
	Parser::MapHppJointType::const_iterator it = jointsMap_.find (jointName);
	if (it == jointsMap_.end ())
	  {
	    Parser::JointPtrType ptr;
	    ptr.reset ();
	    return ptr;
	  }
	return it->second;
      }

      CkitMat4
      Parser::poseToMatrix (::urdf::Pose p)
      {
	CkitMat4 t;

	// Fill rotation part.
	btQuaternion q (p.rotation.x, p.rotation.y,
			p.rotation.z, p.rotation.w);
	btMatrix3x3 rotationMatrix (q);
	for (unsigned i = 0; i < 3; ++i)
	  for (unsigned j = 0; j < 3; ++j)
	    t(i, j) = rotationMatrix[i][j];

	// Fill translation part.
	t(0, 3) = p.position.x;
	t(1, 3) = p.position.y;
	t(2, 3) = p.position.z;
	t(3, 3) = 1.;

	t(3, 0) = 0;
	t(3, 1) = 0;
	t(3, 2) = 0.;

	return t;
      }

      vector3d
      Parser::computeAnklePositionInLocalFrame
      (MapHppJointType::const_iterator& foot,
       MapHppJointType::const_iterator& ankle) const
      {
	matrix4d world_M_foot =
	  foot->second->jrlJoint ()->initialPosition ();
	matrix4d world_M_ankle =
	  ankle->second->jrlJoint ()->initialPosition ();
	matrix4d foot_M_world;
	world_M_foot.Inversion (foot_M_world);

	matrix4d foot_M_ankle = foot_M_world * world_M_ankle;
	return vector3d (foot_M_ankle (0, 3),
			 foot_M_ankle (1, 3),
			 foot_M_ankle (2, 3));
      }

      CkitMat4
      Parser::getPoseInReferenceFrame (const std::string& referenceJointName,
				       const std::string& currentJointName)
      {
	if (referenceJointName == currentJointName)
	  return poseToMatrix
	    (model_.getJoint
	     (currentJointName)->parent_to_joint_origin_transform);

	// Retrieve corresponding joint in URDF tree.
	UrdfJointConstPtrType joint = model_.getJoint (currentJointName);
	if (!joint)
	  {
	    hppDout (error,
		     "Failed to retrieve parent while computing joint position");
	    CkitMat4 result;
	    result.identity ();
	    return result;
	  }

	// Get transform from parent link to joint.
	::urdf::Pose jointToParentTransform =
	    joint->parent_to_joint_origin_transform;

	CkitMat4 transform = poseToMatrix (jointToParentTransform);

	// Get parent joint name.
	std::string parentLinkName = joint->parent_link_name;
	UrdfLinkConstPtrType parentLink = model_.getLink (parentLinkName);

	if (!parentLink)
	  return transform;
	UrdfJointConstPtrType parentJoint = parentLink->parent_joint;
	if (!parentJoint)
	  return transform;

	// Compute previous transformation with current one.
	transform =
	  getPoseInReferenceFrame (referenceJointName,
				   parentJoint->name) * transform;
	return transform;
      }

      Parser::RobotPtrType
      Parser::parse (const std::string& filename)
      {
	resource_retriever::Retriever resourceRetriever;

	resource_retriever::MemoryResource resource =
	  resourceRetriever.get(filename);
	std::string robotDescription;
	robotDescription.resize(resource.size);
	unsigned i = 0;
	for (; i < resource.size; ++i)
	  robotDescription[i] = resource.data.get()[i];
	return parseStream (robotDescription);
      }

      Parser::RobotPtrType
      Parser::parseStream (const std::string& robotDescription)
      {
	// Reset the attributes to avoid problems when loading
	// multiple robots using the same object.
	model_.clear ();
	robot_ = hpp::model::HumanoidRobot::create (model_.getName ());
	rootJoint_.reset ();
	jointsMap_.clear ();

	// Parse urdf model.
	if (!model_.initString (robotDescription))
	  {
	    hppDout (error, "Failed to open URDF file."
		     << " Is the filename location correct?");
	    robot_.reset ();
	    return robot_;
	  }

	// Get names of special joints.
	findSpecialJoints ();

	// Look for joints in the URDF model tree.
	if (!parseJoints ())
	  {
	    hppDout (error, "Could not parse joints.");
	    robot_.reset ();
	    return robot_;
	  }

	// Create the kinematic tree.
	// We iterate over the URDF root joints to connect them to the
	// root link that we added "manually" before. Then we iterate
	// in the whole tree using the connectJoints method.
	boost::shared_ptr<const ::urdf::Link> rootLink = model_.getRoot ();
	if (!rootLink)
	  {
	    hppDout (error, "URDF model is missing a root link");
	    robot_.reset ();
	    return robot_;
	  }

	typedef boost::shared_ptr<const ::urdf::Joint> JointPtr_t;
	if (!connectJoints (rootJoint_))
	  {
	    hppDout (error, "Could not connect joints.");
	    robot_.reset ();
	    return robot_;
	  }

	// Look for special joints and attach them to the model.
	setSpecialJoints ();

	// Add corresponding body (link) to each joint.
	if (!addBodiesToJoints ())
	  {
	    hppDout (error, "Could not add bodies to joints.");
	    robot_.reset ();
	    return robot_;
	  }

	// Initialize dynamic part.
	robot_->initialize();

	// Set model actuated joints. Make sure to call this *after*
	// initializating the structure.
	std::vector<CjrlJoint*> actJointsVect = actuatedJoints ();
	robot_->setActuatedJoints (actJointsVect);

	// Fill gaze position and direction.
	fillGaze ();

	// Here we need to use joints initial positions. Make sure to
	// call this *after* initializating the structure.
	fillHandsAndFeet ();

	// Set default steering method that will be used by roadmap
	// builders.
	robot_->steeringMethodComponent (CkppSMLinearComponent::create ());

	//Set bounds on freeflyer dofs
	setFreeFlyerBounds ();

	return robot_;
      }

    } // end of namespace urdf.
  } // end of namespace model.
} // end of namespace  hpp.
