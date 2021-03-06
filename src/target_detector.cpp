//
// Created by Eric Fang on 7/26/17.
//

#include <landing_module/target_detector.h>

void target_detector::initialize_callbacks() {
//    synchronizer_.registerCallback(boost::bind(&target_detector::topics_callback, this, _1, _2));
    image_sub_ = it_.subscribe("image", 1, &target_detector::topics_callback, this);
    image_pub_ = it_.advertise("/out", 1);
    state_sub_ = nh_.subscribe("mavros/state", 10, &target_detector::state_callback, this);
    pos_sub_ = nh_.subscribe("mavros/local_position/pose", 10, &target_detector::pose_callback, this);
    pos_pub_ = nh_.advertise<geometry_msgs::PoseStamped>("mavros/setpoint_position/local", 10);
    vel_pub_ = nh_.advertise<geometry_msgs::TwistStamped>("mavros/setpoint_velocity/cmd_vel", 10);
    raw_pub_ = nh_.advertise<mavros_msgs::PositionTarget>("mavros/setpoint_raw/local", 10);
}

void target_detector::state_callback(const mavros_msgs::StateConstPtr &stateMsg) {
    current_state = *stateMsg;
}

void target_detector::pose_callback(const geometry_msgs::PoseStampedConstPtr &poseMsg) {
    current_pose = *poseMsg;
}

void target_detector::initialize_uav() {
    ros::Rate rate(20.0);

    while(ros::ok() && !current_state.connected){
        ros::spinOnce();
        rate.sleep();
    }

    geometry_msgs::PoseStamped pose;
    pose.pose.position.x = 0;
    pose.pose.position.y = 0;
    pose.pose.position.z = 0;

    //send a few setpoints before starting
    for(int i = 100; ros::ok() && i > 0; --i){
        pos_pub_.publish(pose);
        ros::spinOnce();
        rate.sleep();
    }

    mavros_msgs::SetMode set_mode_msg;
    set_mode_msg.request.custom_mode = "OFFBOARD";

    mavros_msgs::CommandBool arm_cmd;
    arm_cmd.request.value = true;

    while(ros::ok() && !current_state.armed) {

        if( current_state.mode != "OFFBOARD" &&
            (ros::Time::now() - last_command > ros::Duration(5.0))){
            if( set_mode_client.call(set_mode_msg) &&
                set_mode_msg.response.success) {
                ROS_INFO("Offboard enabled");
            }
            last_command = ros::Time::now();
        }
        else {
            if( !current_state.armed &&
                (ros::Time::now() - last_command > ros::Duration(5.0))){
                if( arming_client.call(arm_cmd) &&
                    arm_cmd.response.success) {
                    ROS_INFO("Vehicle armed");
                }
                last_command = ros::Time::now();
            }
        }

        pos_pub_.publish(pose);
        ros::spinOnce();
        rate.sleep();
    }
}

void target_detector::search_controller() {

    mavros_msgs::PositionTarget raw_pos;
    raw_pos.coordinate_frame = raw_pos.FRAME_LOCAL_NED;
    raw_pos.type_mask = raw_pos.IGNORE_AFX | raw_pos.IGNORE_AFY | raw_pos.IGNORE_AFZ | raw_pos.IGNORE_YAW_RATE;

    geometry_msgs::TwistStamped twist;
//    static double yaw_dir = 45;
    static unsigned search_index = 0;

    twist.twist.linear.x = 0;
    twist.twist.linear.y = 0;
    twist.twist.linear.z = search_altitude - current_pose.pose.position.z;

    if (std::abs(current_pose.pose.position.z - search_altitude) < 0.5) search_mode = true;

    if (ros::Time::now() - last_detection > ros::Duration(10) && search_mode) {
        double desired_position_x = search_position_x + search_pattern[search_index % search_pattern.size()].x;
        double desired_position_y = search_position_y + search_pattern[search_index % search_pattern.size()].y;

        twist.twist.linear.x = desired_position_x - current_pose.pose.position.x;
        twist.twist.linear.y = desired_position_y - current_pose.pose.position.y;

        if (std::abs(current_pose.pose.position.x - desired_position_x) < 1.0 &&
            std::abs(current_pose.pose.position.y - desired_position_y) < 1.0) {
            search_index++;
        }
    }

    if (ros::Time::now() - last_detection < ros::Duration(5) && search_mode) {
        if (gimbal_state.pitch < gimbal_state.pitch_min + 20) {
            double error_x = (target_location.x - (image_size.x/2));
            double error_y = (target_location.y - (image_size.y/2));
            double error_corrected_x = error_x * std::cos(-gimbal_state.yaw * (CV_PI/180.0)) -
                    error_y * std::sin(-gimbal_state.yaw * (CV_PI/180.0));
            double error_corrected_y = error_x * std::sin(-gimbal_state.yaw * (CV_PI/180.0)) +
                                       error_y * std::cos(-gimbal_state.yaw * (CV_PI/180.0));

            twist.twist.linear.x = error_corrected_x * 0.05;
            twist.twist.linear.y = error_corrected_y * 0.05;
        }
        else {
            twist.twist.linear.x = std::cos(-gimbal_state.yaw * (CV_PI/180.0));
            twist.twist.linear.y = std::sin(-gimbal_state.yaw * (CV_PI/180.0));
        }
    }

    vel_pub_.publish(twist);

    if (ros::Time::now() - last_detection > ros::Duration(3) && search_mode) {

        mavros_msgs::CommandLong gimbal_command;

        if (ros::Time::now() - last_command > ros::Duration(0.5)) {

//            if (gimbal_state.yaw == gimbal_state.yaw_max) yaw_dir = -45;
//            else if (gimbal_state.yaw == gimbal_state.yaw_min) yaw_dir = 45;

//            gimbal_state.add_yaw(yaw_dir);
            gimbal_state.pitch = -90;

            gimbal_command.request.command = MAV_CMD_DO_MOUNT_CONTROL;
            gimbal_command.request.param1 = float(gimbal_state.pitch);
            gimbal_command.request.param3 = float(gimbal_state.yaw);
            gimbal_command.request.param7 = MAV_MOUNT_MODE_MAVLINK_TARGETING;

            if (gimbal_command_client.call(gimbal_command) && !int(gimbal_command.response.success)) {
                std::cout << "command failed!" << std::endl;
            }

            last_command = ros::Time::now();
        }
    }

//    geometry_msgs::PoseStamped pose;
//
//    pose.pose.position.x = search_position_x;
//    pose.pose.position.y = search_position_y;
//    pose.pose.position.z = search_altitude;
//    pose.pose.orientation = toQuaternion(0, 0, search_yaw);
//
//    pos_pub_.publish(pose);

}

bool target_detector::detect_target(const cv::Mat &input, const cv::Mat& display) {

    std::vector<cv::Point2f> corners;
    int maxCorners = 10;
    double qualityLevel = 0.01;
    double minDistance = 5;
    int blockSize = 3;
    bool useHarrisDetector = false;
    double k = 0.04;

    cv::goodFeaturesToTrack(input, corners, maxCorners, qualityLevel, minDistance, cv::Mat(),
                            blockSize, useHarrisDetector, k);

    for (auto& corner : corners) {
        if (corner.x < 2 || corner.y < 2 || (corner.x > (input.cols - 2)) || (corner.y > (input.rows - 2))) return false;

        std::vector<int> transitions;
        std::vector<int> pixels;
        for (auto& loc : ring) {
            pixels.push_back(int(input.at<uchar>(corner + loc)));
        }

        double sum = std::accumulate(pixels.begin(), pixels.end(), 0.0);
        double mean = sum / pixels.size();

        for (int i = 1; i < pixels.size(); i++) {
            if (pixels[i] > mean && pixels[i - 1] <= mean) {
                transitions.push_back(i);

            }
            else if (pixels[i] < mean && pixels[i - 1] >= mean) {
                transitions.push_back(i);
            }
        }

        if (transitions.size() == 4) {
            int num_first = (transitions[0] + int(pixels.size() - 1) - transitions[3]);
            int num_second = (transitions[1] - transitions[0]);
            int num_third = (transitions[2] - transitions[1]);
            int num_fourth = (transitions[3] - transitions[2]);

            if (num_first < 2 || num_second < 2 || num_third < 2 || num_fourth < 2) return false;

            int sum_first = std::accumulate(pixels.begin(), pixels.begin() + transitions[0], 0);
            sum_first += std::accumulate(pixels.begin() + transitions[3], pixels.end() - 1, 0);
            int sum_second = std::accumulate(pixels.begin() + transitions[0], pixels.begin() + transitions[1], 0);
            int sum_third = std::accumulate(pixels.begin() + transitions[1], pixels.begin() + transitions[2], 0);
            int sum_fourth = std::accumulate(pixels.begin() + transitions[2], pixels.begin() + transitions[3], 0);

            int mean_first = sum_first / num_first;
            int mean_second = sum_second / num_second;
            int mean_third = sum_third / num_third;
            int mean_fourth = sum_fourth / num_fourth;

            if ((std::abs(mean_first - mean_third) > close_threshold) || (std::abs(mean_second - mean_fourth) > close_threshold)) {
                return false;
            }
            if ((std::abs(mean_first - mean_second) < far_threshold) || (std::abs(mean_third - mean_fourth) < far_threshold)) {
                return false;
            }

            target_location.x = corner.x;
            target_location.y = corner.y;

            cv::circle(display, corner, 3, cv::Scalar(255, 0, 0), -1, 8, 0);
            std::ostringstream text;
            text << transitions.size();
            cv::putText(display, text.str(), corner, fontFace, 0.5, cv::Scalar(255, 0, 0));
            return true;
        }
    }

    return false;
}

void target_detector::track_target(cv::Point target_location, const cv::Mat &image) {

    double aspect_ratio = double(image.cols)/image.rows;
    double v_fov = h_fov/aspect_ratio;
    double yaw_correction = (target_location.x - (image.cols/2))*(h_fov/image.cols)*0.5;
    double pitch_correction = -(target_location.y - (image.rows/2))*(v_fov/image.rows)*0.5;
    gimbal_state.add_yaw(yaw_correction);
    gimbal_state.add_pitch(pitch_correction);

//    std::cout << gimbal_state.yaw << ", " << gimbal_state.pitch << std::endl;

    mavros_msgs::CommandLong gimbal_command;

    gimbal_command.request.command = MAV_CMD_DO_MOUNT_CONTROL;
    gimbal_command.request.param1 = float(gimbal_state.pitch);
    gimbal_command.request.param2 = 0;
    gimbal_command.request.param3 = float(gimbal_state.yaw);
    gimbal_command.request.param7 = MAV_MOUNT_MODE_MAVLINK_TARGETING;

    if (ros::Time::now() - last_command > ros::Duration(0.5)) {

        if (gimbal_command_client.call(gimbal_command) && !int(gimbal_command.response.success)) {
            std::cout << "command failed!" << std::endl;
        }

        last_command = ros::Time::now();
    }
}

void target_detector::topics_callback(/*const geometry_msgs::PoseStampedConstPtr& poseMsg,*/
                                      const sensor_msgs::ImageConstPtr& imageMsg) {
    cv_bridge::CvImagePtr src_gray_ptr;
    cv_bridge::CvImagePtr src_ptr;

    if (current_state.armed && search_mode) {
        try {
            src_gray_ptr = cv_bridge::toCvCopy(imageMsg, sensor_msgs::image_encodings::MONO8);
            src_ptr = cv_bridge::toCvCopy(imageMsg, sensor_msgs::image_encodings::BGR8);
        } catch (cv_bridge::Exception &e) {
            throw std::runtime_error(
                    std::string("cv_bridge exception: ") + std::string(e.what()));

        }

        target_found = detect_target(src_gray_ptr->image, src_ptr->image);

        if (target_found) {
//            std::stringstream image_file_name;
//            std::stringstream patch_file_name;
//            std::stringstream time;
//            std::ofstream text;
//
//            time << ros::Time::now();
//            image_file_name << "/Users/eric1221bday/sandbox/" << time.str() << ".jpg";
//            patch_file_name << "/Users/eric1221bday/sandbox/" << time.str() << "_patch.jpg";
//            text.open("/Users/eric1221bday/sandbox/" + time.str());
//
//            src_gray_ptr->image.at<uchar>(target_location) = 255;
//            cv::Mat success_patch = src_gray_ptr->image(cv::Rect(std::max(int(target_location.x - 9), 0), std::max(int(target_location.y - 9), 0), 20, 20));
//            cv::imwrite(image_file_name.str(), src_ptr->image);
//            cv::imwrite(patch_file_name.str(), success_patch);
//
//            text << current_pose.pose.position.x << ", " << current_pose.pose.position.y << ", " << current_pose.pose.position.z << std::endl;
//            text << gimbal_state.roll << ", " << gimbal_state.pitch << ", " << gimbal_state.yaw << std::endl;
//            text << target_location.x << ", " << target_location.y << std::endl;
//
//            text.close();

//            cv::Mat dst_x, dst_y, dst, abs_dst_x, abs_dst_y, patch_enlarged;
//
//            cv::resize(success_patch, patch_enlarged, cv::Size(32, 32), 0, 0, cv::INTER_NEAREST);
//            cv::GaussianBlur(patch_enlarged, patch_enlarged, cv::Size(3, 3), 0, 0);
//            cv::Scharr(patch_enlarged, dst_x, CV_32F, 1, 0);
//            cv::Scharr(patch_enlarged, dst_y, CV_32F, 0, 1);
//            cv::convertScaleAbs(dst_x, abs_dst_x);
//            cv::convertScaleAbs(dst_y, abs_dst_y);
//            cv::addWeighted(abs_dst_x, 0.5, abs_dst_y, 0.5, 0, dst);
//    //    cv::Mat src_display = src_gray_ptr->image(cv::Rect(600, 350, 100, 100));
//            cv::imshow("Window", dst);
//            cv::imshow("Window2", patch_enlarged);
//            cv::waitKey(3);
        }

        // Output modified video stream
        image_pub_.publish(src_ptr->toImageMsg());

        if (target_found) {
            last_detection = ros::Time::now();
            track_target(target_location, src_ptr->image);
        }
    }


}