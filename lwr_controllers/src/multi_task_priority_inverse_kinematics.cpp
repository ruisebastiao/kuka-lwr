#include <pluginlib/class_list_macros.h>
#include <kdl_parser/kdl_parser.hpp>
#include <math.h>
#include <Eigen/LU>

#include <utils/pseudo_inversion.h>
#include <utils/skew_symmetric.h>
#include <lwr_controllers/multi_task_priority_inverse_kinematics.h>

namespace lwr_controllers 
{
	MultiTaskPriorityInverseKinematics::MultiTaskPriorityInverseKinematics() {}
	MultiTaskPriorityInverseKinematics::~MultiTaskPriorityInverseKinematics() {}

	bool MultiTaskPriorityInverseKinematics::init(hardware_interface::EffortJointInterface *robot, ros::NodeHandle &n)
	{
		nh_ = n;

		// get URDF and name of root and tip from the parameter server
		std::string robot_description, root_name, tip_name;

		if (!ros::param::search(n.getNamespace(),"robot_description", robot_description))
		{
		    ROS_ERROR_STREAM("MultiTaskPriorityInverseKinematics: No robot description (URDF) found on parameter server ("<<n.getNamespace()<<"/robot_description)");
		    return false;
		}

		if (!nh_.getParam("root_name", root_name))
		{
		    ROS_ERROR_STREAM("MultiTaskPriorityInverseKinematics: No root name found on parameter server ("<<n.getNamespace()<<"/root_name)");
		    return false;
		}

		if (!nh_.getParam("tip_name", tip_name))
		{
		    ROS_ERROR_STREAM("MultiTaskPriorityInverseKinematics: No tip name found on parameter server ("<<n.getNamespace()<<"/tip_name)");
		    return false;
		}
	 
		// Get the gravity vector (direction and magnitude)
		KDL::Vector gravity_ = KDL::Vector::Zero();
		gravity_(2) = -9.81;

		// Construct an URDF model from the xml string
		std::string xml_string;

		if (n.hasParam(robot_description))
			n.getParam(robot_description.c_str(), xml_string);
		else
		{
		    ROS_ERROR("Parameter %s not set, shutting down node...", robot_description.c_str());
		    n.shutdown();
		    return false;
		}

		if (xml_string.size() == 0)
		{
			ROS_ERROR("Unable to load robot model from parameter %s",robot_description.c_str());
		    n.shutdown();
		    return false;
		}

		ROS_DEBUG("%s content\n%s", robot_description.c_str(), xml_string.c_str());
		
		// Get urdf model out of robot_description
		urdf::Model model;
		if (!model.initString(xml_string))
		{
		    ROS_ERROR("Failed to parse urdf file");
		    n.shutdown();
		    return false;
		}
		ROS_INFO("Successfully parsed urdf file");
		
		KDL::Tree kdl_tree_;
		if (!kdl_parser::treeFromUrdfModel(model, kdl_tree_))
		{
		    ROS_ERROR("Failed to construct kdl tree");
		    n.shutdown();
		    return false;
		}

		// Populate the KDL chain
		if(!kdl_tree_.getChain(root_name, tip_name, kdl_chain_))
		{
		    ROS_ERROR_STREAM("Failed to get KDL chain from tree: ");
		    ROS_ERROR_STREAM("  "<<root_name<<" --> "<<tip_name);
		    ROS_ERROR_STREAM("  Tree has "<<kdl_tree_.getNrOfJoints()<<" joints");
		    ROS_ERROR_STREAM("  Tree has "<<kdl_tree_.getNrOfSegments()<<" segments");
		    ROS_ERROR_STREAM("  The segments are:");

		    KDL::SegmentMap segment_map = kdl_tree_.getSegments();
		    KDL::SegmentMap::iterator it;

		    for( it=segment_map.begin(); it != segment_map.end(); it++ )
		      ROS_ERROR_STREAM( "    "<<(*it).first);

		  	return false;
		}

		ROS_DEBUG("Number of segments: %d", kdl_chain_.getNrOfSegments());
		ROS_DEBUG("Number of joints in chain: %d", kdl_chain_.getNrOfJoints());

		// Get joint handles for all of the joints in the chain
		for(std::vector<KDL::Segment>::const_iterator it = kdl_chain_.segments.begin()+1; it != kdl_chain_.segments.end(); ++it)
		{
		    joint_handles_.push_back(robot->getHandle(it->getJoint().getName()));
		    ROS_DEBUG("%s", it->getJoint().getName().c_str() );
		}

		ROS_DEBUG(" Number of joints in handle = %lu", joint_handles_.size() );

		PIDs_.resize(kdl_chain_.getNrOfJoints());

		// Parsing PID gains from YAML 
	    std::string pid_ = ("pid_");
	    for (int i = 0; i < joint_handles_.size(); ++i)
		{
		    if (!PIDs_[i].init(ros::NodeHandle(n, pid_ + joint_handles_[i].getName())))
		    {
		        ROS_ERROR("Error initializing the PID for joint %d",i);
		        return false;
		    }
		}

		jnt_to_jac_solver_.reset(new KDL::ChainJntToJacSolver(kdl_chain_));
		id_solver_.reset(new KDL::ChainDynParam(kdl_chain_,gravity_));
		fk_pos_solver_.reset(new KDL::ChainFkSolverPos_recursive(kdl_chain_));

		joint_msr_states_.resize(kdl_chain_.getNrOfJoints());
		joint_des_states_.resize(kdl_chain_.getNrOfJoints());
		tau_cmd_.resize(kdl_chain_.getNrOfJoints());
		J_.resize(kdl_chain_.getNrOfJoints());
		J_star_.resize(kdl_chain_.getNrOfJoints());

		sub_command_ = nh_.subscribe("command_configuration", 1, &MultiTaskPriorityInverseKinematics::command_configuration, this);

		pub_error_ = nh_.advertise<std_msgs::Float64MultiArray>("error", 1000);
		pub_marker_ = nh_.advertise<visualization_msgs::MarkerArray>("marker",1000);

		return true;
	}

	void MultiTaskPriorityInverseKinematics::starting(const ros::Time& time)
	{
		// get joint positions
  		for(int i=0; i < joint_handles_.size(); i++) 
  		{
    		joint_msr_states_.q(i) = joint_handles_[i].getPosition();
    		joint_msr_states_.qdot(i) = joint_handles_[i].getVelocity();
    		joint_des_states_.q(i) = joint_msr_states_.q(i);
    		joint_des_states_.qdot(i) = joint_msr_states_.qdot(i);
    	}

    	I_ = Eigen::Matrix<double,7,7>::Identity(7,7);
    	e_dot_ = Eigen::Matrix<double,6,1>::Zero();

    	cmd_flag_ = 0;
	}

	void MultiTaskPriorityInverseKinematics::update(const ros::Time& time, const ros::Duration& period)
	{
		// get joint positions
  		for(int i=0; i < joint_handles_.size(); i++) 
  		{
    		joint_msr_states_.q(i) = joint_handles_[i].getPosition();
    		joint_msr_states_.qdot(i) = joint_handles_[i].getVelocity();
    	}
    	
    	// clearing error msg before publishing
    	msg_err_.data.clear();

    	if (cmd_flag_)
    	{
    		// resetting P and qdot(t=0) for the highest priority task
    		P_ = I_;	
    		SetToZero(joint_des_states_.qdot);

    		for (int index = 0; index < ntasks_; index++)
    		{
		    	// computing Jacobian
		    	jnt_to_jac_solver_->JntToJac(joint_msr_states_.q,J_,links_index_[index]);

		    	// computing forward kinematics
		    	fk_pos_solver_->JntToCart(joint_msr_states_.q,x_,links_index_[index]);

		    	// setting marker parameters
		    	set_marker(x_,index,msg_id_);

		    	// computing end-effector position/orientation error w.r.t. desired frame
		    	x_err_ = diff(x_,x_des_[index]);

		    	for(int i = 0; i < e_dot_.size(); i++)
		    	{
		    		e_dot_(i) = x_err_(i);
	    			msg_err_.data.push_back(e_dot_(i));
		    	}

		    	// computing (J[i]*P[i-1])^pinv
		    	J_star_.data = J_.data*P_;
		    	pseudo_inverse(J_star_.data,J_pinv_);

		    	// computing q_dot (qdot(i) = qdot[i-1] + (J[i]*P[i-1])^pinv*(x_err[i] - J[i]*qdot[i-1]))
		    	joint_des_states_.qdot.data = joint_des_states_.qdot.data + J_pinv_*(e_dot_ - J_.data*joint_des_states_.qdot.data);

		    	// stop condition
		    	if (!on_target_flag_[index])
		    	{
			    	if (Equal(x_,x_des_[index],0.01))
			    	{
			    		ROS_INFO("Task %d on target",index);
			    		on_target_flag_[index] = true;
			    		if (index == (ntasks_ - 1))
			    			cmd_flag_ = 0;
			    	}
			    }

		    	// updating P_ (it mustn't make use of the damped pseudo inverse)
		    	pseudo_inverse(J_star_.data,J_pinv_,false);
		    	P_ = P_ - J_pinv_*J_star_.data;
		    }

		    // integrating q_dot -> getting q (Euler method)
		    for (int i = 0; i < joint_handles_.size(); i++)
		    	joint_des_states_.q(i) += period.toSec()*joint_des_states_.qdot(i);			
    	}

    	// set controls for joints
    	for (int i = 0; i < joint_handles_.size(); i++)
    	{
    		tau_cmd_(i) = PIDs_[i].computeCommand(joint_des_states_.q(i) - joint_msr_states_.q(i),joint_des_states_.qdot(i) - joint_msr_states_.qdot(i),period);
    		joint_handles_[i].setCommand(tau_cmd_(i));
    	}

    	// publishing markers for visualization in rviz
    	pub_marker_.publish(msg_marker_);
    	msg_id_++;

	    // publishing error for all tasks as an array of ntasks*6
	    pub_error_.publish(msg_err_);
	    ros::spinOnce();

	}

	void MultiTaskPriorityInverseKinematics::command_configuration(const lwr_controllers::MultiPriorityTask::ConstPtr &msg)
	{
		if (msg->links.size() == msg->tasks.size()/6)
		{
			ntasks_ = msg->links.size();
			ROS_INFO("Number of tasks: %d",ntasks_);
			// Dynamically resize desired postures and links index when a message arrives
			x_des_.resize(ntasks_);
			links_index_.resize(ntasks_);
			on_target_flag_.resize(ntasks_);
			msg_marker_.markers.resize(ntasks_);
			msg_id_ = 0;

			for (int i = 0; i < ntasks_; i++)
			{
					if (msg->links[i] == -1)	// adjust index
						links_index_[i] = msg->links[i];
					else if (msg->links[i] >= 1 && msg->links[i] <=joint_handles_.size())
						links_index_[i] = msg->links[i] + 1;
					else
					{
						ROS_INFO("Links index must be within 1 and %ld. (-1 is end-effector)",joint_handles_.size());
						return;
					}
					
					x_des_[i] = KDL::Frame(
									KDL::Rotation::RPY(msg->tasks[i*6 + 3],
													   msg->tasks[i*6 + 4],
													   msg->tasks[i*6 + 5]),
									KDL::Vector(msg->tasks[i*6],
												msg->tasks[i*6 + 1],
												msg->tasks[i*6 + 2]));

					on_target_flag_[i] = false;
			}

			cmd_flag_ = 1;
		}	
		else
		{
			ROS_INFO("The number of links index and tasks must be the same");
			ROS_INFO("Tasks parameters are [x,y,x,roll,pitch,yaw]");
			return;
		}
		
	}

	void MultiTaskPriorityInverseKinematics::set_marker(KDL::Frame x, int index, int id)
	{			
				sstr_.str("");
				sstr_.clear();

				if (links_index_[index] == -1)
					sstr_<<"end_effector";		
				else
					sstr_<<"link_"<<(links_index_[index]-1);


				msg_marker_.markers[index].header.frame_id = "world";
				msg_marker_.markers[index].header.stamp = ros::Time();
				msg_marker_.markers[index].ns = sstr_.str();
				msg_marker_.markers[index].id = id;
				msg_marker_.markers[index].type = visualization_msgs::Marker::SPHERE;
				msg_marker_.markers[index].action = visualization_msgs::Marker::ADD;
				msg_marker_.markers[index].pose.position.x = x.p(0);
				msg_marker_.markers[index].pose.position.y = x.p(1);
				msg_marker_.markers[index].pose.position.z = x.p(2);
				msg_marker_.markers[index].pose.orientation.x = 0.0;
				msg_marker_.markers[index].pose.orientation.y = 0.0;
				msg_marker_.markers[index].pose.orientation.z = 0.0;
				msg_marker_.markers[index].pose.orientation.w = 1.0;
				msg_marker_.markers[index].scale.x = 0.01;
				msg_marker_.markers[index].scale.y = 0.01;
				msg_marker_.markers[index].scale.z = 0.01;
				msg_marker_.markers[index].color.a = 1.0;
				msg_marker_.markers[index].color.r = 0.0;
				msg_marker_.markers[index].color.g = 1.0;
				msg_marker_.markers[index].color.b = 0.0;	
	}
}

PLUGINLIB_EXPORT_CLASS(lwr_controllers::MultiTaskPriorityInverseKinematics, controller_interface::ControllerBase)
