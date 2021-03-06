#include "actionlib/server/simple_action_server.h"
#include "hexapod_control/SetJointAction.h"
#include "hexapod_control/SolveFKPose.h"
#include "hexapod_control/SolveIKPose.h"
#include "hexapod_control/Pose.h"
#include "sensor_msgs/JointState.h"
#include "std_msgs/Float64.h"

class SetJointAction
{
public:
    SetJointAction(std::string name) :
        server(node, name, boost::bind(&SetJointAction::executeCB, this, _1), false),
        actionName(name)
    {
        this->node = node;

        ROS_INFO("Subscribing to Joint States...");
        this->jointStateSubscriber = node.subscribe("/hexapod/joint_states", 10, &SetJointAction::jointStatesCB, this);

        ROS_INFO("Publishing to Joint Controllers");
        this->hipJointPublisher = node.advertise<std_msgs::Float64>("/hexapod/leg_R2/hip_joint_position_controller/command", 1);
        this->kneeJointPublisher = node.advertise<std_msgs::Float64>("/hexapod/leg_R2/knee_joint_position_controller/command", 1);
        this->ankleJointPublisher = node.advertise<std_msgs::Float64>("/hexapod/leg_R2/ankle_joint_position_controller/command", 1);

        ROS_INFO("Subscribing to FKPoseSolver service...");
        this->fkClient = node.serviceClient<hexapod_control::SolveFKPose>("/hexapod/leg_R2/fk");

        ROS_INFO("Starting...");
        server.start();
    }

    ~SetJointAction()
    {
        this->node.shutdown();
    }

    void executeCB(const hexapod_control::SetJointGoalConstPtr& goal)
    {
        // Set goal
        this->hipTarget = goal->goal[0];
        this->kneeTarget = goal->goal[1];
        this->ankleTarget = goal->goal[2];
        this->eps = goal->eps;

        // Command joints
        std_msgs::Float64 hipJointCommand, kneeJointCommand, ankleJointCommand;
        hipJointCommand.data = this->hipTarget;
        kneeJointCommand.data = this->kneeTarget;
        ankleJointCommand.data = this->ankleTarget;
        hipJointPublisher.publish(hipJointCommand);
        kneeJointPublisher.publish(kneeJointCommand);
        ankleJointPublisher.publish(ankleJointCommand);
        this->actionFeedback.targets = goal->goal;

        // Get current time
        double start = ros::Time::now().toSec();

        // Make sure goal state is not current state
        if (calculateJointError() < this->eps)
        {
            this->actionResult.result = current_state.position;
            this->actionResult.error = calculateJointError();
            this->actionResult.time = 0;
            server.setSucceeded(actionResult);
            return;
        }

        // Wait for joints to start moving
        ros::Rate rate(50);
        while(isRobotIdle())
        {
            rate.sleep();
        }

        // Publish feedback
        while (!isRobotIdle() && server.isActive())
        {
            // Check for preemption
            if (server.isPreemptRequested() || !ros::ok())
            {
                ROS_INFO("Joint action preempted.");
                server.setPreempted();
                return;
            }

            this->actionFeedback.positions = current_state.position;
            this->actionFeedback.error = calculateJointError();
            this->actionFeedback.time = ros::Time::now().toSec() - start;
            server.publishFeedback(this->actionFeedback);

            rate.sleep();
        }

        // Publish result
        this->actionResult.result = current_state.position;
        this->actionResult.error = calculateJointError();
        this->actionResult.time = ros::Time::now().toSec() - start;
        if (this->actionResult.error < eps)
        {
            this->actionResult.error_code = 0;
        }
        else
        {
            this->actionResult.error_code = -2;
        }
        
        server.setSucceeded(this->actionResult);
    }

    hexapod_control::SolveFKPoseResponse getCurrentPose()
    {
        // Send FK request to service
        hexapod_control::SolveFKPose fkMsg;
        fkMsg.request.joint_positions = this->current_state.position;
        fkClient.call(fkMsg);

        return fkMsg.response;
    }

    double calculateJointError()
    {
        double hipError = this->hipTarget - current_state.position[0];
        double kneeError = this->kneeTarget - current_state.position[1];
        double ankleError = this->ankleTarget - current_state.position[2];
        return sqrt(pow(hipError, 2.0) + pow(kneeError, 2.0) + pow(ankleError, 2.0));
    }

    bool isRobotIdle()
    {
        std::vector<double> velocity = this->current_state.velocity;
        double magnitude = 0;
        for (int i = 0; i < velocity.size(); ++i)
        {
            magnitude += pow(velocity[i], 2.0);
        }
        double jointError = calculateJointError();
        return magnitude < 0.05 && jointError < eps;
    }

    void jointStatesCB(const sensor_msgs::JointStateConstPtr& msg)
    {
        this->temp = *msg.get();
        int hipIndex, kneeIndex, ankleIndex;
        for (int i = 0; i < temp.name.size(); ++i)
        {
            std::string name_i = temp.name[i];
            if (name_i.find("R2") != std::string::npos)
            {
                if (name_i.find("hip") != std::string::npos)
                {
                    hipIndex = i;
                }
                else if (name_i.find("knee") != std::string::npos)
                {
                    kneeIndex = i;
                }
                else if (name_i.find("ankle") != std::string::npos)
                {
                    ankleIndex = i;
                }
            }
        }

        this->current_state.name     = {temp.name[hipIndex],     temp.name[kneeIndex],     temp.name[ankleIndex]};
        this->current_state.position = {temp.position[hipIndex], temp.position[kneeIndex], temp.position[ankleIndex]};
        this->current_state.velocity = {temp.velocity[hipIndex], temp.velocity[kneeIndex], temp.velocity[ankleIndex]};
        this->current_state.effort   = {temp.effort[hipIndex],   temp.effort[kneeIndex],   temp.effort[ankleIndex]};
    }

private:
    std::string actionName;
    ros::NodeHandle node;
    actionlib::SimpleActionServer<hexapod_control::SetJointAction> server;
    hexapod_control::SetJointFeedback actionFeedback;
    hexapod_control::SetJointResult actionResult;
    ros::ServiceClient fkClient;
    ros::Subscriber jointStateSubscriber;
    ros::Publisher hipJointPublisher;
    ros::Publisher kneeJointPublisher;
    ros::Publisher ankleJointPublisher;
    sensor_msgs::JointState current_state;
    sensor_msgs::JointState temp;
    double hipTarget;
    double kneeTarget;
    double ankleTarget;
    double eps;
};

int main(int argc, char **argv)
{
    ROS_INFO("Starting Joint Action Server...");
    ros::init(argc, argv, "leg_R2_joint_action");

    SetJointAction actionServer("leg_R2_joint_action");
    ros::spin();
    return 0;
}