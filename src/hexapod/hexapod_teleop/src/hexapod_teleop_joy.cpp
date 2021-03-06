#include "ros/ros.h"
#include "std_msgs/Bool.h"
#include "geometry_msgs/Twist.h"
#include "sensor_msgs/Joy.h"
#include "boost/thread/mutex.hpp"
#include "boost/thread/thread.hpp"

// Joy package button mappings
// http://wiki.ros.org/joy#Microsoft_Xbox_360_Wireless_Controller_for_Linux

class TeleopHexapod
{
public:
    TeleopHexapod() :
        Lx(0),  // L stick X-axis
        Ly(1),  // L stick Y-axis
        Rx(2),  // R stick X-axis
        B(1)    // B button
    {
        ROS_INFO("Subscribing to Joystick");
        this->joystickSubscriber = node.subscribe<sensor_msgs::Joy>("joy", 10, &TeleopHexapod::joystickCB, this);

        ROS_INFO("Publishing Teleop Data...");
        this->twistPublisher = node.advertise<geometry_msgs::Twist>("hexapod/teleop/twist", 1);
        this->buttonPublisher = node.advertise<std_msgs::Bool>("hexapod/teleop/button", 1);

        double publish_rate = 0.1; // 10Hz
        timer = node.createTimer(ros::Duration(publish_rate), boost::bind(&TeleopHexapod::publish, this));

        B_state = false;
        last_B_state = false;
        dirty = false;

        ROS_INFO("Teleop controller ready.\n");
    }

    ~TeleopHexapod()
    {
        this->node.shutdown();
    }

private:
    ros::NodeHandle node;
    ros::Subscriber joystickSubscriber;
    ros::Publisher twistPublisher;
    ros::Publisher buttonPublisher;
    int Lx, Ly, Rx, B;
    bool B_state, last_B_state, dirty;
    geometry_msgs::Twist last_twist_msg;
    std_msgs::Bool last_button_msg;
    ros::Timer timer;
    boost::mutex publish_mutex;

    void joystickCB(const sensor_msgs::JoyConstPtr& msg)
    {
        geometry_msgs::Twist twist_msg;
        twist_msg.linear.x = -msg->axes[Lx];
        twist_msg.linear.y = msg->axes[Ly];
        twist_msg.angular.x = -msg->axes[Rx];
        last_twist_msg = twist_msg;

        B_state = msg->buttons[B];
        if (B_state && !last_B_state)
        {
            dirty = true;
        }
        last_B_state = B_state;
    }

    void publish()
    {
        boost::mutex::scoped_lock lock(publish_mutex);

        twistPublisher.publish(last_twist_msg);
        
        std_msgs::Bool button_msg;
        if (dirty)
        {
            button_msg.data = true;
            dirty = false;
        }
        else
        {
            button_msg.data = false;
        }
        buttonPublisher.publish(button_msg); // TODO: change to byte for more button data?

        ROS_INFO("Lx: %f, Ly: %f, Rx: %f, B: %d",
            last_twist_msg.linear.x, last_twist_msg.linear.y, last_twist_msg.angular.x, button_msg.data);
    }
};

int main(int argc, char** argv)
{
    ROS_INFO("Starting Teleop Joystick...");
    ros::init(argc, argv, "hexapod_teleop_joystick");
    ROS_INFO("Initialized ros...");

    TeleopHexapod teleop_hexapod;
    ROS_INFO("Spinning node...");
    ros::spin();
    return 0;
}