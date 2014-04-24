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

/// \file src/util.cc
///
/// \brief Implementation of utility functions.

#include <hpp/util/debug.hh>
#include <hpp/model/urdf/util.hh>

namespace hpp
{
  namespace model
  {
    namespace urdf
    {
      model::DevicePtr_t
      loadRobotModel (const std::string& robotName,
		      const std::string& rootJointType,
		      const std::string& package,
		      const std::string& modelName,
		      const std::string& urdfSuffix,
		      const std::string& srdfSuffix)
      {
	model::DevicePtr_t robot (model::Device::create (robotName));
	hpp::model::urdf::Parser urdfParser (rootJointType, robot);
	hpp::model::srdf::Parser srdfParser;

	std::string urdfPath = "package://" + package + "/urdf/"
	  + modelName + urdfSuffix + ".urdf";
	std::string srdfPath = "package://" + package + "/srdf/"
	  + modelName + srdfSuffix + ".srdf";

	// Build robot model from URDF.
	urdfParser.parse (urdfPath);
	hppDout (notice, "Finished parsing URDF file.");
	// Set Collision Check Pairs
	srdfParser.parse (urdfPath, srdfPath, robot);
	hppDout (notice, "Finished parsing SRDF file.");
	return robot;
      }

      model::HumanoidRobotPtr_t
      loadHumanoidModel (const std::string& robotName,
			 const std::string& rootJointType,
			 const std::string& package,
			 const std::string& modelName,
			 const std::string& urdfSuffix,
			 const std::string& srdfSuffix)
      {
	model::HumanoidRobotPtr_t robot
	  (model::HumanoidRobot::create (robotName));
	hpp::model::urdf::Parser urdfParser (rootJointType, robot);
	hpp::model::srdf::Parser srdfParser;

	std::string urdfPath = "package://" + package + "/urdf/"
	  + modelName + urdfSuffix + ".urdf";
	std::string srdfPath = "package://" + package + "/srdf/"
	  + modelName + srdfSuffix + ".srdf";

	// Build robot model from URDF.
	urdfParser.parse (urdfPath);
	hppDout (notice, "Finished parsing URDF file.");
	// Look for special joints and attach them to the model.
	urdfParser.setSpecialJoints ();
	// Fill gaze position and direction.
	urdfParser.fillGaze ();


	// Set Collision Check Pairs
	srdfParser.parse (urdfPath, srdfPath, robot);
	hppDout (notice, "Finished parsing SRDF file.");
	return robot;
      }

      model::DevicePtr_t
      loadUrdfModel (const std::string& robotName,
		     const std::string& rootJointType,
		     const std::string& package,
		     const std::string& filename)
      {
	model::DevicePtr_t robot (model::Device::create (robotName));
	hpp::model::urdf::Parser urdfParser (rootJointType, robot);

	std::string urdfPath = "package://" + package + "/urdf/"
	  + filename + ".urdf";

	// Build robot model from URDF.
	urdfParser.parse (urdfPath);
	hppDout (notice, "Finished parsing URDF file.");
	return robot;
      }
    } // end of namespace urdf.
  } // end of namespace model.
} // end of namespace  hpp.
