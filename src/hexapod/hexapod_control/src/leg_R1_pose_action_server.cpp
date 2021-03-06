#include "actionlib/server/simple_action_server.h"
#include "actionlib/client/simple_action_client.h"
#include "hexapod_control/SetPoseAction.h"
#include "hexapod_control/SetJointAction.h"
#include "hexapod_control/SolveFKPose.h"
#include "hexapod_control/SolveIKPose.h"
#include "hexapod_control/Pose.h"
#include "sensor_msgs/JointState.h"
#include "std_msgs/Float64.h"

class SetPoseAction
{
public:
    SetPoseAction(std::string name) :
        server(node, name, boost::bind(&SetPoseAction::executeCB, this, _1), false),
        client("leg_R1_joint_action", true),
        actionName(name)
    {
        this->node = node;

        ROS_INFO("Waiting for Joint State Server...");
        this->client.waitForServer(ros::Duration(30));

        ROS_INFO("Subscribing to Joint States...");
        this->jointStateSubscriber = node.subscribe("/hexapod/joint_states", 10, &SetPoseAction::jointStatesCB, this);

        ROS_INFO("Subscribing to IKPoseSolver service...");
        this->ikClient = node.serviceClient<hexapod_control::SolveIKPose>("/hexapod/leg_R1/ik");

        ROS_INFO("Subscribing to FKPoseSolver service...");
        this->fkClient = node.serviceClient<hexapod_control::SolveFKPose>("/hexapod/leg_R1/fk");

        ROS_INFO("Starting...");
        server.start();
    }

    ~SetPoseAction()
    {
        this->node.shutdown();
    }

    void executeCB(const hexapod_control::SetPoseGoalConstPtr &goal)
    {
        // Run IK to get joint positions
        hexapod_control::SolveIKPose ikMsg;
        ikMsg.request.initial_state = this->current_state.position;
        ikMsg.request.goal = goal->goal;

        this->ikClient.call(ikMsg);

        // IK Solver failed
        if (ikMsg.response.result < 0)
        {
            this->actionResult.error_code = -1;
            server.setSucceeded(this->actionResult);
            return;
        }
        this->targetx = goal->goal.x;
        this->targety = goal->goal.y;
        this->targetz = goal->goal.z;
        this->hipTarget = ikMsg.response.solution[0];
        this->kneeTarget = ikMsg.response.solution[1];
        this->ankleTarget = ikMsg.response.solution[2];
        this->eps = goal->eps;
        this->actionFeedback.target = goal->goal;

        // Send joint positions to joint action client
        hexapod_control::SetJointGoal jointAction;
        jointAction.goal = ikMsg.response.solution;
        jointAction.eps = goal->eps;
        this->client.sendGoal(jointAction,
            boost::bind(&SetPoseAction::publishResult, this, _1, _2),
            boost::bind(&SetPoseAction::activeCB, this),
            boost::bind(&SetPoseAction::publishFeedback, this, _1));

        // Get current time
        double start = ros::Time::now().toSec();

        // Wait for joints to start moving
        ros::Rate rate(50);
        while(isRobotIdle())
        {
            if (server.isPreemptRequested() || !ros::ok())
            {
                ROS_INFO("Pose action preempted.");
                server.setPreempted();
                return;
            }
            rate.sleep();
        }

        // Check for preemption
        while (!isRobotIdle())
        {
            if (server.isPreemptRequested() || !ros::ok())
            {
                ROS_INFO("Pose action preempted.");
                server.setPreempted();
                return;
            }

            rate.sleep();
        }

        // Publish result
        hexapod_control::SolveFKPoseResponse fkResponse = getCurrentPose(this->current_state.position);
        if (fkResponse.result == 0)
        {
            this->actionResult.final_pose = fkResponse.solution;
            this->actionResult.error = calculateTaskError(fkResponse);
            this->actionResult.time = ros::Time::now().toSec() - start;
            if (this->actionResult.error < this->eps)
            {
                this->actionResult.error_code = 0;
            }
            else
            {
                this->actionResult.error_code = -2;
            }
        }
        else
        {
            this->actionResult.error_code = -3;
        }
        server.setSucceeded(this->actionResult);
    }

    void publishFeedback(const hexapod_control::SetJointFeedback::ConstPtr& jointFeedback)
    {
        // Get current pose
        hexapod_control::SolveFKPoseResponse fkResponse = getCurrentPose(jointFeedback->positions);
        if (fkResponse.result == 0)
        {
            this->actionFeedback.current_pose = fkResponse.solution;
            this->actionFeedback.error = calculateTaskError(fkResponse);
            this->actionFeedback.time = jointFeedback->time;
            server.publishFeedback(this->actionFeedback);
        }
    }

    void publishResult(const actionlib::SimpleClientGoalState& state, 
        const hexapod_control::SetJointResult::ConstPtr& jointResult)
    {
        
    }

    void activeCB()
    {

    }

    hexapod_control::SolveFKPoseResponse getCurrentPose(std::vector<double> config)
    {
        // Send FK request to service
        hexapod_control::SolveFKPose fkMsg;
        fkMsg.request.joint_positions = config;
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

    double calculateTaskError(hexapod_control::SolveFKPoseResponse fkResponse)
    {
        double dx = this->targetx - fkResponse.solution.x;
        double dy = this->targety - fkResponse.solution.y;
        double dz = this->targetz - fkResponse.solution.z;
        return sqrt(pow(dx, 2.0) + pow(dy, 2.0) + pow(dz, 2.0));
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
        return magnitude < 0.01 && jointError < 0.1;
    }

    void jointStatesCB(const sensor_msgs::JointStateConstPtr& msg)
    {
        sensor_msgs::JointState temp = *msg.get();
        int hipIndex, kneeIndex, ankleIndex;
        for (int i = 0; i < temp.name.size(); ++i)
        {
            std::string name_i = temp.name[i];
            if (name_i.find("R1") != std::string::npos)
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

        this->current_state.name = {temp.name[hipIndex], temp.name[kneeIndex], temp.name[ankleIndex]};
        this->current_state.position = {temp.position[hipIndex], temp.position[kneeIndex], temp.position[ankleIndex]};
        this->current_state.velocity = {temp.velocity[hipIndex], temp.velocity[kneeIndex], temp.velocity[ankleIndex]};
        this->current_state.effort = {temp.effort[hipIndex], temp.effort[kneeIndex], temp.effort[ankleIndex]};
    }

private:
    std::string actionName;
    ros::NodeHandle node;
    actionlib::SimpleActionServer<hexapod_control::SetPoseAction>  server;
    actionlib::SimpleActionClient<hexapod_control::SetJointAction> client;
    hexapod_control::SetPoseFeedback actionFeedback;
    hexapod_control::SetPoseResult actionResult;
    ros::ServiceClient ikClient;
    ros::ServiceClient fkClient;
    ros::Subscriber jointStateSubscriber;
    sensor_msgs::JointState current_state;
    double hipTarget;
    double kneeTarget;
    double ankleTarget;
    double targetx;
    double targety;
    double targetz;
    double eps;
};

int main(int argc, char **argv)
{
    ROS_INFO("Starting Pose Action Server...");
    ros::init(argc, argv, "leg_R1_pose_action");
    ROS_INFO("Initialized ros...");

    SetPoseAction actionServer("leg_R1_pose_action");
    ROS_INFO("Spinning node...");
    ros::spin();
    return 0;
}
