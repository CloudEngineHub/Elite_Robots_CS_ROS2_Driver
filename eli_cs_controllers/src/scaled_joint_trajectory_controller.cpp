#include <memory>
#include <vector>

#include "eli_cs_controllers/scaled_joint_trajectory_controller.hpp"

#include "lifecycle_msgs/msg/state.hpp"

namespace ELITE_CS_CONTROLLER {

controller_interface::CallbackReturn ScaledJointTrajectoryController::on_init() {
    // Create the parameter listener and get the parameters
    scaled_param_listener_ = std::make_shared<scaled_joint_trajectory_controller::ParamListener>(get_node());
    scaled_params_ = scaled_param_listener_->get_params();

    return JointTrajectoryController::on_init();
}

controller_interface::InterfaceConfiguration ScaledJointTrajectoryController::state_interface_configuration() const {
    controller_interface::InterfaceConfiguration conf;
    conf = JointTrajectoryController::state_interface_configuration();
    conf.names.push_back(scaled_params_.speed_scaling_interface_name);

    return conf;
}

controller_interface::CallbackReturn ScaledJointTrajectoryController::on_activate(const rclcpp_lifecycle::State& state) {
    TimeData time_data;
    time_data.time = get_node()->now();
    time_data.period = rclcpp::Duration::from_nanoseconds(0);
    time_data.uptime = get_node()->now();
    time_data_.initRT(time_data);
    return JointTrajectoryController::on_activate(state);
}

controller_interface::return_type ScaledJointTrajectoryController::update(const rclcpp::Time& time,
                                                                          const rclcpp::Duration& period) {
    if (state_interfaces_.back().get_name() == scaled_params_.speed_scaling_interface_name) {
        scaling_factor_ = state_interfaces_.back().get_value();
    } else {
        RCLCPP_ERROR(get_node()->get_logger(), "Speed scaling interface (%s) not found in hardware interface.",
                     scaled_params_.speed_scaling_interface_name.c_str());
    }

    if (get_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE) {
        return controller_interface::return_type::OK;
    }

    auto compute_error_for_joint = [&](JointTrajectoryPoint& error, int index, const JointTrajectoryPoint& current,
                                       const JointTrajectoryPoint& desired) {
        // error defined as the difference between current and desired
        if (joints_angle_wraparound_[index]) {
            // if desired, the shortest_angular_distance is calculated, i.e., the error is
            //  normalized between -pi<error<pi
            error.positions[index] = angles::shortest_angular_distance(current.positions[index], desired.positions[index]);
        } else {
            error.positions[index] = desired.positions[index] - current.positions[index];
        }
        if (has_velocity_state_interface_ && (has_velocity_command_interface_ || has_effort_command_interface_)) {
            error.velocities[index] = desired.velocities[index] - current.velocities[index];
        }
        if (has_acceleration_state_interface_ && has_acceleration_command_interface_) {
            error.accelerations[index] = desired.accelerations[index] - current.accelerations[index];
        }
    };

    // don't update goal after we sampled the trajectory to avoid any racecondition
    const auto active_goal = *rt_active_goal_.readFromRT();

    // Check if a new external message has been received from nonRT threads
    auto current_external_msg = traj_external_point_ptr_->get_trajectory_msg();
    auto new_external_msg = traj_msg_external_point_ptr_.readFromRT();
    // Discard, if a goal is pending but still not active (somewhere stuck in goal_handle_timer_)
    if (current_external_msg != *new_external_msg && !(rt_has_pending_goal_ && !active_goal)) {
        fill_partial_goal(*new_external_msg);
        sort_to_local_joint_order(*new_external_msg);
        // TODO(denis): Add here integration of position and velocity
        traj_external_point_ptr_->update(*new_external_msg);
    }

    // current state update
    state_current_.time_from_start.set__sec(0);
    read_state_from_state_interfaces(state_current_);

    // currently carrying out a trajectory
    if (has_active_trajectory()) {
        // Adjust time with scaling factor
        TimeData time_data;
        time_data.time = time;
        rcl_duration_value_t t_period = (time_data.time - time_data_.readFromRT()->time).nanoseconds();
        time_data.period = rclcpp::Duration::from_nanoseconds(scaling_factor_ * t_period);
        time_data.uptime = time_data_.readFromRT()->uptime + time_data.period;
        rclcpp::Time traj_time = time_data_.readFromRT()->uptime + rclcpp::Duration::from_nanoseconds(t_period);
        time_data_.reset();
        time_data_.initRT(time_data);

        bool first_sample = false;
        // if sampling the first time, set the point before you sample
        if (!traj_external_point_ptr_->is_sampled_already()) {
            first_sample = true;
            if (params_.open_loop_control) {
                traj_external_point_ptr_->set_point_before_trajectory_msg(traj_time, last_commanded_state_);
            } else {
                traj_external_point_ptr_->set_point_before_trajectory_msg(traj_time, state_current_);
            }
        }

        // find segment for current timestamp
        joint_trajectory_controller::TrajectoryPointConstIter start_segment_itr, end_segment_itr;
        const bool valid_point =
            traj_external_point_ptr_->sample(traj_time, interpolation_method_, state_desired_, start_segment_itr, end_segment_itr);

        if (valid_point) {
            const rclcpp::Time traj_start = traj_external_point_ptr_->time_from_start();
            // this is the time instance
            // - started with the first segment: when the first point will be reached (in the future)
            // - later: when the point of the current segment was reached
            const rclcpp::Time segment_time_from_start = traj_start + start_segment_itr->time_from_start;
            // time_difference is
            // - negative until first point is reached
            // - counting from zero to time_from_start of next point
            double time_difference = time.seconds() - segment_time_from_start.seconds();
            bool tolerance_violated_while_moving = false;
            bool outside_goal_tolerance = false;
            bool within_goal_time = true;
            const bool before_last_point = end_segment_itr != traj_external_point_ptr_->end();

            // have we reached the end, are not holding position, and is a timeout configured?
            // Check independently of other tolerances
            if (!before_last_point && !rt_is_holding_ && cmd_timeout_ > 0.0 &&
                time_difference > cmd_timeout_) {
                RCLCPP_WARN(get_node()->get_logger(), "Aborted due to command timeout");

                traj_msg_external_point_ptr_.reset();
                traj_msg_external_point_ptr_.initRT(set_hold_position());
            }

            // Check state/goal tolerance
            for (size_t index = 0; index < dof_; ++index) {
                compute_error_for_joint(state_error_, index, state_current_, state_desired_);

                // Always check the state tolerance on the first sample in case the first sample
                // is the last point
                if ((before_last_point || first_sample) &&
                    !check_state_tolerance_per_joint(state_error_, index, default_tolerances_.state_tolerance[index], false) &&
                    !rt_is_holding_) {
                    tolerance_violated_while_moving = true;
                }
                // past the final point, check that we end up inside goal tolerance
                if (!before_last_point &&
                    !check_state_tolerance_per_joint(state_error_, index, default_tolerances_.goal_state_tolerance[index], false) &&
                    !rt_is_holding_) {
                    outside_goal_tolerance = true;

                    if (default_tolerances_.goal_time_tolerance != 0.0) {
                        if (time_difference > default_tolerances_.goal_time_tolerance) {
                            within_goal_time = false;
                        }
                    }
                }
            }

            // set values for next hardware write() if tolerance is met
            if (!tolerance_violated_while_moving && within_goal_time) {
                if (use_closed_loop_pid_adapter_) {
                    // Update PIDs
                    for (auto i = 0ul; i < dof_; ++i) {
                        tmp_command_[i] = (state_desired_.velocities[i] * ff_velocity_scale_[i]) +
                                          pids_[i]->computeCommand(state_error_.positions[i], state_error_.velocities[i],
                                                                   (uint64_t)period.nanoseconds());
                    }
                }

                // set values for next hardware write()
                if (has_position_command_interface_) {
                    assign_interface_from_point(joint_command_interface_[0], state_desired_.positions);
                }
                if (has_velocity_command_interface_) {
                    if (use_closed_loop_pid_adapter_) {
                        assign_interface_from_point(joint_command_interface_[1], tmp_command_);
                    } else {
                        assign_interface_from_point(joint_command_interface_[1], state_desired_.velocities);
                    }
                }
                if (has_acceleration_command_interface_) {
                    assign_interface_from_point(joint_command_interface_[2], state_desired_.accelerations);
                }
                if (has_effort_command_interface_) {
                    assign_interface_from_point(joint_command_interface_[3], tmp_command_);
                }

                // store the previous command. Used in open-loop control mode
                last_commanded_state_ = state_desired_;
            }

            if (active_goal) {
                // send feedback
                auto feedback = std::make_shared<FollowJTrajAction::Feedback>();
                feedback->header.stamp = time;
                feedback->joint_names = params_.joints;

                feedback->actual = state_current_;
                feedback->desired = state_desired_;
                feedback->error = state_error_;
                active_goal->setFeedback(feedback);

                // check abort
                if (tolerance_violated_while_moving) {
                    auto result = std::make_shared<FollowJTrajAction::Result>();
                    result->set__error_code(FollowJTrajAction::Result::PATH_TOLERANCE_VIOLATED);
                    active_goal->setAborted(result);
                    // TODO(matthew-reynolds): Need a lock-free write here
                    // See https://github.com/ros-controls/ros2_controllers/issues/168
                    rt_active_goal_.writeFromNonRT(RealtimeGoalHandlePtr());
                    rt_has_pending_goal_ = false;

                    RCLCPP_WARN(get_node()->get_logger(), "Aborted due to state tolerance violation");

                    traj_msg_external_point_ptr_.reset();
                    traj_msg_external_point_ptr_.initRT(set_hold_position());
                } else if (!before_last_point) {
                    if (!outside_goal_tolerance) {
                        auto res = std::make_shared<FollowJTrajAction::Result>();
                        res->set__error_code(FollowJTrajAction::Result::SUCCESSFUL);
                        active_goal->setSucceeded(res);
                        // TODO(matthew-reynolds): Need a lock-free write here
                        // See https://github.com/ros-controls/ros2_controllers/issues/168
                        rt_active_goal_.writeFromNonRT(RealtimeGoalHandlePtr());
                        rt_has_pending_goal_ = false;

                        RCLCPP_INFO(get_node()->get_logger(), "Goal reached, success!");

                        traj_msg_external_point_ptr_.reset();
                        traj_msg_external_point_ptr_.initRT(set_success_trajectory_point());
                    } else if (!within_goal_time) {
                        auto result = std::make_shared<FollowJTrajAction::Result>();
                        result->set__error_code(FollowJTrajAction::Result::GOAL_TOLERANCE_VIOLATED);
                        active_goal->setAborted(result);
                        // TODO(matthew-reynolds): Need a lock-free write here
                        // See https://github.com/ros-controls/ros2_controllers/issues/168
                        rt_active_goal_.writeFromNonRT(RealtimeGoalHandlePtr());
                        rt_has_pending_goal_ = false;

                        RCLCPP_WARN(get_node()->get_logger(), "Aborted due goal_time_tolerance exceeding by %f seconds",
                                    time_difference);

                        traj_msg_external_point_ptr_.reset();
                        traj_msg_external_point_ptr_.initRT(set_hold_position());
                    }
                }
            } else if (tolerance_violated_while_moving && !rt_has_pending_goal_) {
                // we need to ensure that there is no pending goal -> we get a race condition otherwise
                RCLCPP_ERROR(get_node()->get_logger(), "Holding position due to state tolerance violation");

                traj_msg_external_point_ptr_.reset();
                traj_msg_external_point_ptr_.initRT(set_hold_position());
            } else if (!before_last_point && !within_goal_time && !rt_has_pending_goal_) {
                RCLCPP_ERROR(get_node()->get_logger(), "Exceeded goal_time_tolerance: holding position...");

                traj_msg_external_point_ptr_.reset();
                traj_msg_external_point_ptr_.initRT(set_hold_position());
            }
            // else, run another cycle while waiting for outside_goal_tolerance
            // to be satisfied (will stay in this state until new message arrives)
            // or outside_goal_tolerance violated within the goal_time_tolerance
        }
    }

    publish_state(state_desired_, state_current_, state_error_);
    return controller_interface::return_type::OK;
}

}  // namespace ELITE_CS_CONTROLLER

#include "pluginlib/class_list_macros.hpp"
PLUGINLIB_EXPORT_CLASS(ELITE_CS_CONTROLLER::ScaledJointTrajectoryController, controller_interface::ControllerInterface)
