#include "boost/bind.hpp"
#include "boost/ref.hpp"
#include "darknet_ros_msgs/BoundingBox.h"
#include "darknet_ros_msgs/BoundingBoxes.h"
#include "geometry_msgs/Point.h"
#include "geometry_msgs/PoseStamped.h"
#include "ignition/msgs.hh"
#include "message_filters/subscriber.h"
#include "message_filters/time_synchronizer.h"
#include "pcl/common/centroid.h"
#include "pcl/point_cloud.h"
#include "pcl_conversions/pcl_conversions.h"
#include "ros/ros.h"
#include "sensor_msgs/PointCloud2.h"
#include "subt_communication_broker/subt_communication_client.h"
#include "subt_ign/CommonTypes.hh"
#include "subt_ign/protobuf/artifact.pb.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.h"
#include "tf2_ros/transform_broadcaster.h"
#include "tf2_ros/transform_listener.h"
#include "ToTransformStamped.h"

std::string robot_name;
std::string camera_frame;
std::string object_frame;
std::string artifact_origin_frame;
std::string rgbd_pc_topic;
std::string darknet_bb_topic;

// define the fundamental traits of an artifact (type and location),
// and report the most recently found artifact
struct Artifact
{
  subt::ArtifactType type;
  geometry_msgs::Point location;
};
Artifact artifact_to_report;
bool have_an_artifact_to_report = false;

void BaseStationCallback(
  const std::string & srcAddress,
  const std::string & dstAddress,
  const uint32_t dstPort,
  const std::string & data);

void ReportArtifacts(const ros::TimerEvent &, subt::CommsClient & commsClient);

void ProcessDetection(
  const sensor_msgs::PointCloud2::ConstPtr & cloud_msg,
  const darknet_ros_msgs::BoundingBoxes::ConstPtr & bb_msg,
  tf2_ros::Buffer & tf_buffer);

sensor_msgs::PointCloud2 CropPointCloud(
  const sensor_msgs::PointCloud2::ConstPtr & original_pc,
  const darknet_ros_msgs::BoundingBox & bb);

geometry_msgs::PoseStamped GetCentroid(sensor_msgs::PointCloud2 & original_pc);

int main(int argc, char *argv[])
{
  ros::init(argc, argv, "artifact_reporter");
  ros::NodeHandle nh;

  ros::NodeHandle private_nh("~");
  private_nh.param("robot_name", robot_name, std::string("anymal_b"));
  private_nh.param("camera_frame", camera_frame, robot_name + "/base/camera_front");
  private_nh.param("artifact_origin_frame", artifact_origin_frame, std::string("artifact_origin"));
  private_nh.param("rgbd_pc_topic", rgbd_pc_topic, "/" + robot_name + "/downward_realsense/points");
  private_nh.param("darknet_bb_topic", darknet_bb_topic, std::string("/darknet_ros/bounding_boxes"));
  ROS_INFO_STREAM(
    "artifact_reporter values..." << std::endl <<
    "robot_name: " << robot_name << std::endl <<
    "camera_frame: " << camera_frame << std::endl <<
    "artifact_origin_frame: " << artifact_origin_frame << std::endl <<
    "rgbd_pc_topic: " << rgbd_pc_topic << std::endl <<
    "darknet_bb_topic: " << darknet_bb_topic << std::endl);

  tf2_ros::Buffer tf_buffer;
  tf2_ros::TransformListener tf_listener(tf_buffer);

  // set up communications with the base station for artifact reporting
  subt::CommsClient commsClient(robot_name);
  commsClient.Bind(&BaseStationCallback, robot_name);

  // found artifacts will be attempted to be sent periodically through a timer
  ros::Timer timer = nh.createTimer(ros::Duration(1.0), boost::bind(&ReportArtifacts, _1, boost::ref(commsClient)));

  // when darknet detects an object, we need the corresponding point cloud data from the RGBD camera
  // so that we can determine the location of this object
  message_filters::Subscriber<sensor_msgs::PointCloud2> pc_sub(nh, rgbd_pc_topic, 1);
  message_filters::Subscriber<darknet_ros_msgs::BoundingBoxes> bb_sub(nh, darknet_bb_topic, 1);
  message_filters::TimeSynchronizer<sensor_msgs::PointCloud2, darknet_ros_msgs::BoundingBoxes> sync(pc_sub, bb_sub, 10);
  sync.registerCallback(boost::bind(&ProcessDetection, _1, _2, boost::ref(tf_buffer)));

  ros::spin();
}

void BaseStationCallback(
  const std::string & srcAddress,
  const std::string & dstAddress,
  const uint32_t dstPort,
  const std::string & data)
{
  subt::msgs::ArtifactScore res;
  if (!res.ParseFromString(data))
  {
    ROS_ERROR("ArtifactReporter::BaseStationCallback(): error deserializing message.");
  }

  geometry_msgs::Point location;
  location.x = res.artifact().pose().position().x();
  location.y = res.artifact().pose().position().y();
  location.z = res.artifact().pose().position().z();

  ROS_INFO_STREAM("Artifact at location " <<
    location.x << ", " << location.y << ", " << location.z <<
    " was received by the base station");
  
  // since the artifact we saved has been reported, we no longer have an artifact to report
  // until we detect another one
  have_an_artifact_to_report = false;
}

void ReportArtifacts(const ros::TimerEvent &, subt::CommsClient & commsClient)
{
  if (!have_an_artifact_to_report)
  {
    return;
  }

  auto location = artifact_to_report.location;
  ignition::msgs::Pose pose;
  pose.mutable_position()->set_x(location.x);
  pose.mutable_position()->set_y(location.y);
  pose.mutable_position()->set_z(location.z);

  // fill the type and pose
  subt::msgs::Artifact artifact;
  artifact.set_type(static_cast<uint32_t>(artifact_to_report.type));
  artifact.mutable_pose()->CopyFrom(pose);

  // serialize the artifact
  std::string serializedData;
  if (!artifact.SerializeToString(&serializedData))
  {
    ROS_ERROR_STREAM("ArtifactReporter::ReportArtifact(): Error serializing message\n" << artifact.DebugString());
  }

  // report the artifact
  commsClient.SendTo(serializedData, subt::kBaseStationName);
}

void ProcessDetection(
  const sensor_msgs::PointCloud2::ConstPtr & cloud_msg,
  const darknet_ros_msgs::BoundingBoxes::ConstPtr & bb_msg,
  tf2_ros::Buffer & tf_buffer)
{
  for (const auto & box : bb_msg->bounding_boxes)
  {
    // take the centroid of the points in the bounding box to get the artifact's location
    // (we'll need to crop the original point cloud to just the points in the bounding box)
    auto cropped_pc = CropPointCloud(cloud_msg, box);
    auto centroid = GetCentroid(cropped_pc);

    // perform the necessary transforms to get the artifact's location with respect to
    // the artifact origin instead of the camera
    auto tf_stamped = ToTransformStamped(centroid, camera_frame, object_frame);
    auto scoring_pose = tf_buffer.transform<geometry_msgs::PoseStamped>(
      centroid, artifact_origin_frame, ros::Duration(1.0));

    if (box.Class == "Backpack")
    {
      artifact_to_report.type = subt::ArtifactType::TYPE_BACKPACK;
      artifact_to_report.location.x = scoring_pose.pose.position.x;
      artifact_to_report.location.y = scoring_pose.pose.position.y;
      artifact_to_report.location.z = scoring_pose.pose.position.z;
      have_an_artifact_to_report = true;

      ROS_INFO_STREAM("Detected a backapack! Location w.r.t "
        << artifact_origin_frame << " : "
        << artifact_to_report.location.x << ", "
        << artifact_to_report.location.y << ", "
        << artifact_to_report.location.z
        << " (x,y,z)");
    }

    if (box.Class == "Survivor")
    {
      artifact_to_report.type = subt::ArtifactType::TYPE_RESCUE_RANDY;
      artifact_to_report.location.x = scoring_pose.pose.position.x;
      artifact_to_report.location.y = scoring_pose.pose.position.y;
      artifact_to_report.location.z = scoring_pose.pose.position.z;
      have_an_artifact_to_report = true;

      ROS_INFO_STREAM("Detected a survivor! Location w.r.t "
        << artifact_origin_frame << " : "
        << artifact_to_report.location.x << ", "
        << artifact_to_report.location.y << ", "
        << artifact_to_report.location.z
        << " (x,y,z)");
    }

    if (box.Class == "Cell Phone")
    {
      artifact_to_report.type = subt::ArtifactType::TYPE_PHONE;
      artifact_to_report.location.x = scoring_pose.pose.position.x;
      artifact_to_report.location.y = scoring_pose.pose.position.y;
      artifact_to_report.location.z = scoring_pose.pose.position.z;
      have_an_artifact_to_report = true;

      ROS_INFO_STREAM("Detected a cell phone! Location w.r.t "
        << artifact_origin_frame << " : "
        << artifact_to_report.location.x << ", "
        << artifact_to_report.location.y << ", "
        << artifact_to_report.location.z
        << " (x,y,z)");
    }

    if (box.Class == "Fire Extinguisher")
    {
      artifact_to_report.type = subt::ArtifactType::TYPE_EXTINGUISHER;
      artifact_to_report.location.x = scoring_pose.pose.position.x;
      artifact_to_report.location.y = scoring_pose.pose.position.y;
      artifact_to_report.location.z = scoring_pose.pose.position.z;
      have_an_artifact_to_report = true;

      ROS_INFO_STREAM("Detected a fire extinguisher! Location w.r.t "
        << artifact_origin_frame << " : "
        << artifact_to_report.location.x << ", "
        << artifact_to_report.location.y << ", "
        << artifact_to_report.location.z
        << " (x,y,z)");
    }

    if (box.Class == "Drill")
    {
      artifact_to_report.type = subt::ArtifactType::TYPE_DRILL;
      artifact_to_report.location.x = scoring_pose.pose.position.x;
      artifact_to_report.location.y = scoring_pose.pose.position.y;
      artifact_to_report.location.z = scoring_pose.pose.position.z;
      have_an_artifact_to_report = true;

      ROS_INFO_STREAM("Detected a drill! Location w.r.t "
        << artifact_origin_frame << " : "
        << artifact_to_report.location.x << ", "
        << artifact_to_report.location.y << ", "
        << artifact_to_report.location.z
        << " (x,y,z)");
    }
  }
}

sensor_msgs::PointCloud2 CropPointCloud(
  const sensor_msgs::PointCloud2::ConstPtr & original_pc,
  const darknet_ros_msgs::BoundingBox & bb)
{
  // create a new PointCloud2 msg that will only contain the points in the bounding box
  sensor_msgs::PointCloud2 modified_pc;
  modified_pc.header = original_pc->header;
  modified_pc.fields = original_pc->fields;
  modified_pc.is_bigendian = original_pc->is_bigendian;
  modified_pc.point_step = original_pc->point_step;
  modified_pc.is_dense = original_pc->is_dense;  
  modified_pc.height = bb.ymax - bb.ymin + 1;
  modified_pc.width = bb.xmax - bb.xmin + 1;
  modified_pc.row_step = modified_pc.width * modified_pc.point_step;

  // perform the cropping
  for (size_t r = bb.ymin; r <= bb.ymax; ++r)
  {
    for (size_t c = bb.xmin; c <= bb.xmax; ++c)
    {
      size_t start = (r * original_pc->row_step) + (c * original_pc->point_step);
      size_t end = start + original_pc->point_step;
      for (size_t i = start; i < end; ++i)
      {
        modified_pc.data.push_back(original_pc->data[i]);
      }
    }
  }

  return modified_pc;
}

geometry_msgs::PoseStamped GetCentroid(sensor_msgs::PointCloud2 & original_pc)
{
  // Convert to PCL data type
  auto cloud = std::make_shared<pcl::PCLPointCloud2>();
  pcl_conversions::toPCL(original_pc, *cloud);
  pcl::PointCloud<pcl::PointXYZ> pclObj;
  pcl::fromPCLPointCloud2(*cloud, pclObj);

  // Save all points in the point cloud to a centroid object
  pcl::CentroidPoint<pcl::PointXYZ> centroid;
  for (auto &p : pclObj.points)
  {
    if (isinf(p.x) || isinf(p.y) || isinf(p.z))
    {
      continue;
    }
    centroid.add(p);
  }

  // extract the centroid
  pcl::PointXYZ location;
  centroid.get(location);

  // convert the centroid to a PoseStamped type so that it's easier to
  // get the location with respect to the artifact origin via tf
  geometry_msgs::PoseStamped p;
  p.header.frame_id = camera_frame;
  p.header.stamp = ros::Time::now();
  p.pose.position.x = location.x;
  p.pose.position.y = location.y;
  p.pose.position.z = location.z;
  p.pose.orientation.x = 0.0;
  p.pose.orientation.y = 0.0;
  p.pose.orientation.z = 0.0;
  p.pose.orientation.w = 1.0;

  return p;
}