////////////////////////////////////////////////////////////////////////////////
// Copyright 2019 FZI Research Center for Information Technology
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
////////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
/*!\file    cartesian_motion_controller.hpp
 *
 * \author  Stefan Scherzinger <scherzin@fzi.de>
 * \date    2017/07/27
 *
 */
//-----------------------------------------------------------------------------

#ifndef CARTESIAN_MOTION_CONTROLLER_HPP_INCLUDED
#define CARTESIAN_MOTION_CONTROLLER_HPP_INCLUDED

// Project
#include <cartesian_motion_controller/cartesian_motion_controller.h>

// Other
#include <algorithm>

namespace cartesian_motion_controller
{

template <class HardwareInterface>
CartesianMotionController<HardwareInterface>::
CartesianMotionController()
: Base::CartesianControllerBase(), listener_(buffer_)
{
}

template <class HardwareInterface>
bool CartesianMotionController<HardwareInterface>::
init(HardwareInterface* hw, ros::NodeHandle& nh)
{
  Base::init(hw,nh);

  if (!nh.getParam("target_frame_topic",m_target_frame_topic))
  {
    m_target_frame_topic = "target_frame";
    ROS_WARN_STREAM("Failed to load "
        << nh.getNamespace() + "/target_frame_topic"
        << " from parameter server. Will default to: "
        << nh.getNamespace() + '/' + m_target_frame_topic);
  }

  m_target_frame_subscr = nh.subscribe(
      m_target_frame_topic,
      3,
      &CartesianMotionController<HardwareInterface>::targetFrameCallback,
      this);

  return true;
}

template <class HardwareInterface>
void CartesianMotionController<HardwareInterface>::
starting(const ros::Time& time)
{
  // Reset simulation with real joint state
  Base::starting(time);
  m_current_frame = Base::m_ik_solver->getEndEffectorPose();

  // Start where we are
  m_target_frame = m_current_frame;
}

template <class HardwareInterface>
void CartesianMotionController<HardwareInterface>::
stopping(const ros::Time& time)
{
}

template <>
void CartesianMotionController<hardware_interface::VelocityJointInterface>::
stopping(const ros::Time& time)
{
    // Stop drifting by sending zero joint velocities
    Base::computeJointControlCmds(ctrl::Vector6D::Zero(), ros::Duration(0));
    Base::writeJointControlCmds();
}

template <class HardwareInterface>
void CartesianMotionController<HardwareInterface>::
update(const ros::Time& time, const ros::Duration& period)
{
  // Synchronize the internal model and the real robot
  Base::m_ik_solver->synchronizeJointPositions(Base::m_joint_handles);

  // Forward Dynamics turns the search for the according joint motion into a
  // control process. So, we control the internal model until we meet the
  // Cartesian target motion. This internal control needs some simulation time
  // steps.
  for (int i = 0; i < Base::m_iterations; ++i)
  {
    // The internal 'simulation time' is deliberately independent of the outer
    // control cycle.
    ros::Duration internal_period(0.02);

    // Compute the motion error = target - current.
    ctrl::Vector6D error = computeMotionError();

    // Turn Cartesian error into joint motion
    Base::computeJointControlCmds(error,internal_period);
  }

  // Write final commands to the hardware interface
  Base::writeJointControlCmds();
}

template <class HardwareInterface>
ctrl::Vector6D CartesianMotionController<HardwareInterface>::
computeMotionError()
{
  // Compute motion error wrt robot_base_link
  m_current_frame = Base::m_ik_solver->getEndEffectorPose();

  // Transformation from target -> current corresponds to error = target - current
  KDL::Frame error_kdl;
  error_kdl.M = m_target_frame.M * m_current_frame.M.Inverse();
  error_kdl.p = m_target_frame.p - m_current_frame.p;

  // Use Rodrigues Vector for a compact representation of orientation errors
  // Only for angles within [0,Pi)
  KDL::Vector rot_axis = KDL::Vector::Zero();
  double angle    = error_kdl.M.GetRotAngle(rot_axis);   // rot_axis is normalized
  double distance = error_kdl.p.Normalize();

  // Clamp maximal tolerated error.
  // The remaining error will be handled in the next control cycle.
  // Note that this is also the maximal offset that the
  // cartesian_compliance_controller can use to build up a restoring stiffness
  // wrench.
  const double max_angle = 1.0;
  const double max_distance = 1.0;
  angle    = std::clamp(angle,-max_angle,max_angle);
  distance = std::clamp(distance,-max_distance,max_distance);

  // Scale errors to allowed magnitudes
  rot_axis = rot_axis * angle;
  error_kdl.p = error_kdl.p * distance;

  // Reassign values
  ctrl::Vector6D error;
  error(0) = error_kdl.p.x();
  error(1) = error_kdl.p.y();
  error(2) = error_kdl.p.z();
  error(3) = rot_axis(0);
  error(4) = rot_axis(1);
  error(5) = rot_axis(2);

  return error;
}

template <class HardwareInterface>
void CartesianMotionController<HardwareInterface>::
targetFrameCallback(const geometry_msgs::PoseStamped& target)
{
  KDL::Frame target_frame_T_robot_base_link = KDL::Frame::Identity();

  if (target.header.frame_id != Base::m_robot_base_link)
  {
    // Support for floating-base systems: If a transform exists between the
    // target's frame_id and the robot_base_link, we will use this looked-up
    // transform, treated as a constant transform for this control step, to
    // define the target frame_id by multiplying the target with the inverse
    // of the looked-up frame thereby allowing control in the robot_base_link
    // reference frame

    // Look up transform; if not found, warn & return
    try {
      geometry_msgs::TransformStamped transform =
          buffer_.lookupTransform(Base::m_robot_base_link, target.header.frame_id,
                                  target.header.stamp, ros::Duration(0.005));  // TODO: hard-coded dt to 5ms
      ROS_INFO_STREAM_THROTTLE(3, "Retrieved transform from [" << target.header.frame_id << "] to [" << Base::m_robot_base_link << "]: [" << transform.transform.translation.x << ", " << transform.transform.translation.y << ", " << transform.transform.translation.z << "], [" << transform.transform.rotation.x << ", " << transform.transform.rotation.y << ", " << transform.transform.rotation.z << ", " << transform.transform.rotation.w << "]");

      target_frame_T_robot_base_link = KDL::Frame(
        KDL::Rotation::Quaternion(
          transform.transform.rotation.x,
          transform.transform.rotation.y,
          transform.transform.rotation.z,
          transform.transform.rotation.w),
        KDL::Vector(
          transform.transform.translation.x,
          transform.transform.translation.y,
          transform.transform.translation.z));
    } catch (const tf2::TransformException &ex) {
      ROS_WARN_STREAM_THROTTLE(3, "Got target pose in wrong reference frame. Expected: "
          << Base::m_robot_base_link << " but got "
          << target.header.frame_id << ". Tried looking up frame but failed: " << ex.what());
      return;
    }
  }

  m_target_frame = target_frame_T_robot_base_link * KDL::Frame(
      KDL::Rotation::Quaternion(
        target.pose.orientation.x,
        target.pose.orientation.y,
        target.pose.orientation.z,
        target.pose.orientation.w),
      KDL::Vector(
        target.pose.position.x,
        target.pose.position.y,
        target.pose.position.z));
}

} // namespace

#endif
