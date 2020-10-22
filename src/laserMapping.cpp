// This is an advanced implementation of the algorithm described in the
// following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

// Modifier: Livox               dev@livoxtech.com

// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
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
#include <omp.h>
#include <math.h>
#include <fstream>
#include <unistd.h>
#include <Python.h>
#include <Eigen/Core>
#include <opencv/cv.h>
#include <common_lib.h>
#include <nav_msgs/Odometry.h>
#include <opencv2/core/eigen.hpp>
#include <visualization_msgs/Marker.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/io/pcd_io.h>

#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_broadcaster.h>
#include <fast_lio/States.h>
#include <geometry_msgs/Vector3.h>
#include "Exp_mat.h"

#ifndef DEPLOY
#include "matplotlibcpp.h"
namespace plt = matplotlibcpp;
#endif

#define INIT_TIME           (3.0)
#define LASER_POINT_COV     (0.0010)
#define NUM_MATCH_POINTS    (5)
#define NUM_MAX_ITERATIONS  (15)
#define LASER_FRAME_INTEVAL (0.1)

typedef pcl::PointXYZI PointType;

std::string root_dir = ROOT_DIR;

float timeLaserCloudSurfLast   = 0;
float timeLaserCloudFullRes    = 0;

int iterCount = 0;
int laserCloudCenWidth  = 10;
int laserCloudCenHeight = 5;
int laserCloudCenDepth  = 10;
const int laserCloudWidth  = 21;
const int laserCloudHeight = 11;
const int laserCloudDepth  = 21;
const int laserCloudNum = laserCloudWidth * laserCloudHeight * laserCloudDepth;//4851

int laserCloudValidInd[125];
int laserCloudSurroundInd[125];
int laserCloudValidNum    = 0;
int laserCloudSurroundNum = 0;

/// IMU relative variables
std::mutex mtx_buffer;
std::condition_variable sig_buffer;
pcl::PointCloud<PointType> v_pcl;
bool b_exit = false;
bool b_reset = false;
bool b_first = true;

/// Buffers for measurements
double lidar_end_time = 0.0;
double last_timestamp_lidar = -1;
double last_timestamp_imu   = -1;

std::deque<sensor_msgs::PointCloud2::ConstPtr> lidar_buffer;
std::deque<sensor_msgs::Imu::ConstPtr> imu_buffer;
std::deque<fast_lio::StatesConstPtr> pose_buffer;
std::deque<sensor_msgs::PointCloud2> LaserCloudSurfaceBuff;

//surf feature in map
pcl::PointCloud<PointType>::Ptr laserCloudSurfFromMap(new pcl::PointCloud<PointType>());

std::deque< fast_lio::States > rot_kp_imu_buff;

//all points
pcl::PointCloud<PointType>::Ptr laserCloudFullRes2(new pcl::PointCloud<PointType>());
pcl::PointCloud<PointType>::Ptr laserCloudSurfArray[laserCloudNum];
pcl::PointCloud<pcl::PointXYZRGB>::Ptr laserCloudFullResColor(new pcl::PointCloud<pcl::PointXYZRGB>());
pcl::KdTreeFLANN<PointType>::Ptr kdtreeSurfFromMap(new pcl::KdTreeFLANN<PointType>());

//estimated rotation and translation;
Eigen::Matrix3d R_global_cur(Eigen::Matrix3d::Identity());
Eigen::Vector3d T_global_cur(0, 0, 0);
Eigen::Vector3d V_global_cur(0, 0, 0);

//final iteration resdual
double cube_len = 0.0;

//project lidar frame to world
void pointAssociateToMap(PointType const * const pi, PointType * const po)
{
    Eigen::Vector3d p_body(pi->x, pi->y, pi->z);
    Eigen::Vector3d p_global(R_global_cur * (p_body + Lidar_offset_to_IMU) + T_global_cur);
    
    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->intensity = pi->intensity;
}

void RGBpointAssociateToMap(PointType const * const pi, pcl::PointXYZRGB * const po)
{
    Eigen::Vector3d p_body(pi->x, pi->y, pi->z);
    Eigen::Vector3d p_global(R_global_cur * (p_body + Lidar_offset_to_IMU) + T_global_cur);
    
    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    //po->intensity = pi->intensity;

    float intensity = pi->intensity;
    intensity = intensity - std::floor(intensity);

    int reflection_map = intensity*10000;

    //std::cout<<"DEBUG reflection_map "<<reflection_map<<std::endl;

    if (reflection_map < 30)
    {
        int green = (reflection_map * 255 / 30);
        po->r = 0;
        po->g = green & 0xff;
        po->b = 0xff;
    }
    else if (reflection_map < 90)
    {
        int blue = (((90 - reflection_map) * 255) / 60);
        po->r = 0x0;
        po->g = 0xff;
        po->b = blue & 0xff;
    }
    else if (reflection_map < 150)
    {
        int red = ((reflection_map-90) * 255 / 60);
        po->r = red & 0xff;
        po->g = 0xff;
        po->b = 0x0;
    }
    else
    {
        int green = (((255-reflection_map) * 255) / (255-150));
        po->r = 0xff;
        po->g = green & 0xff;
        po->b = 0;
    }
}

void lasermap_fov_segment()
{
    laserCloudValidNum    = 0;
    laserCloudSurroundNum = 0;
    PointType pointOnYAxis;
    pointOnYAxis.x = LIDAR_SP_LEN;
    pointOnYAxis.y = 0.0;
    pointOnYAxis.z = 0.0;
    pointAssociateToMap(&pointOnYAxis, &pointOnYAxis);

    int centerCubeI = int((T_global_cur(0) + 0.5 * cube_len) / cube_len) + laserCloudCenWidth;
    int centerCubeJ = int((T_global_cur(1) + 0.5 * cube_len) / cube_len) + laserCloudCenHeight;
    int centerCubeK = int((T_global_cur(2) + 0.5 * cube_len) / cube_len) + laserCloudCenDepth;

    if (T_global_cur(0) + 0.5 * cube_len < 0) centerCubeI--;
    if (T_global_cur(1) + 0.5 * cube_len < 0) centerCubeJ--;
    if (T_global_cur(2) + 0.5 * cube_len < 0) centerCubeK--;

    while (centerCubeI < 3)
    {
        for (int j = 0; j < laserCloudHeight; j++)
        {
            for (int k = 0; k < laserCloudDepth; k++)
            {
                int i = laserCloudWidth - 1;

                pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                        laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];

                for (; i >= 1; i--) {
                    laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                            laserCloudSurfArray[i - 1 + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
                }
                laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                        laserCloudCubeSurfPointer;
                laserCloudCubeSurfPointer->clear();
            }
        }
        centerCubeI++;
        laserCloudCenWidth++;
    }

    while (centerCubeI >= laserCloudWidth - 3) {
        for (int j = 0; j < laserCloudHeight; j++) {
            for (int k = 0; k < laserCloudDepth; k++) {
                int i = 0;
                pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                        laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];

                for (; i < laserCloudWidth - 1; i++)
                {
                    laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                            laserCloudSurfArray[i + 1 + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
                }

                laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                        laserCloudCubeSurfPointer;
                laserCloudCubeSurfPointer->clear();
            }
        }

        centerCubeI--;
        laserCloudCenWidth--;
    }

    while (centerCubeJ < 3) {
        for (int i = 0; i < laserCloudWidth; i++) {
            for (int k = 0; k < laserCloudDepth; k++) {
                int j = laserCloudHeight - 1;
                pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                        laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];

                for (; j >= 1; j--) {
                    laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                            laserCloudSurfArray[i + laserCloudWidth * (j - 1) + laserCloudWidth * laserCloudHeight*k];
                }
                laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                        laserCloudCubeSurfPointer;
                laserCloudCubeSurfPointer->clear();
            }
        }

        centerCubeJ++;
        laserCloudCenHeight++;
    }

    while (centerCubeJ >= laserCloudHeight - 3) {
        for (int i = 0; i < laserCloudWidth; i++) {
            for (int k = 0; k < laserCloudDepth; k++) {
                int j = 0;
                pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                        laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];

                for (; j < laserCloudHeight - 1; j++) {
                    laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                            laserCloudSurfArray[i + laserCloudWidth * (j + 1) + laserCloudWidth * laserCloudHeight*k];
                }
                laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                        laserCloudCubeSurfPointer;
                laserCloudCubeSurfPointer->clear();
            }
        }

        centerCubeJ--;
        laserCloudCenHeight--;
    }

    while (centerCubeK < 3) {
        for (int i = 0; i < laserCloudWidth; i++) {
            for (int j = 0; j < laserCloudHeight; j++) {
                int k = laserCloudDepth - 1;
                pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                        laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];

                for (; k >= 1; k--) {
                    laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                            laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight*(k - 1)];
                }
                laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                        laserCloudCubeSurfPointer;
                laserCloudCubeSurfPointer->clear();
            }
        }

        centerCubeK++;
        laserCloudCenDepth++;
    }

    while (centerCubeK >= laserCloudDepth - 3)
    {
        for (int i = 0; i < laserCloudWidth; i++)
        {
            for (int j = 0; j < laserCloudHeight; j++)
            {
                int k = 0;
                pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                        laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k];
            
                for (; k < laserCloudDepth - 1; k++)
                {
                    laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                            laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight*(k + 1)];
                }

                laserCloudSurfArray[i + laserCloudWidth * j + laserCloudWidth * laserCloudHeight * k] =
                        laserCloudCubeSurfPointer;
                laserCloudCubeSurfPointer->clear();
            }
        }

        centerCubeK--;
        laserCloudCenDepth--;
    }

    for (int i = centerCubeI - 2; i <= centerCubeI + 2; i++) 
    {
        for (int j = centerCubeJ - 2; j <= centerCubeJ + 2; j++) 
        {
            for (int k = centerCubeK - 2; k <= centerCubeK + 2; k++) 
            {
                if (i >= 0 && i < laserCloudWidth &&
                        j >= 0 && j < laserCloudHeight &&
                        k >= 0 && k < laserCloudDepth) 
                {

                    float centerX = cube_len * (i - laserCloudCenWidth);
                    float centerY = cube_len * (j - laserCloudCenHeight);
                    float centerZ = cube_len * (k - laserCloudCenDepth);

                    float check1, check2;
                    float squaredSide1, squaredSide2;
                    float ang_cos = 1;

                    bool isInLaserFOV = false;

                    for (int ii = -1; ii <= 1; ii += 2) 
                    {
                        for (int jj = -1; jj <= 1; jj += 2) 
                        {
                            for (int kk = -1; (kk <= 1) && (!isInLaserFOV); kk += 2) 
                            {

                                float cornerX = centerX + 0.5 * cube_len * ii;
                                float cornerY = centerY + 0.5 * cube_len * jj;
                                float cornerZ = centerZ + 0.5 * cube_len * kk;

                                squaredSide1 = (T_global_cur(0) - cornerX)
                                        * (T_global_cur(0) - cornerX)
                                        + (T_global_cur(1) - cornerY)
                                        * (T_global_cur(1) - cornerY)
                                        + (T_global_cur(2) - cornerZ)
                                        * (T_global_cur(2) - cornerZ);

                                squaredSide2 = (pointOnYAxis.x - cornerX) * (pointOnYAxis.x - cornerX)
                                        + (pointOnYAxis.y - cornerY) * (pointOnYAxis.y - cornerY)
                                        + (pointOnYAxis.z - cornerZ) * (pointOnYAxis.z - cornerZ);

                                ang_cos = fabs(squaredSide1 <= 3) ? 1.0 :
                                    (LIDAR_SP_LEN * LIDAR_SP_LEN + squaredSide1 - squaredSide2) / (2 * LIDAR_SP_LEN * sqrt(squaredSide1));
                                
                                if(ang_cos > 0.7) isInLaserFOV = true;
                            }
                        }
                    }

                    if(!isInLaserFOV)
                    {
                        float cornerX = centerX;
                        float cornerY = centerY;
                        float cornerZ = centerZ;

                        squaredSide1 = (T_global_cur(0) - cornerX)
                                * (T_global_cur(0) - cornerX)
                                + (T_global_cur(1) - cornerY)
                                * (T_global_cur(1) - cornerY)
                                + (T_global_cur(2) - cornerZ)
                                * (T_global_cur(2) - cornerZ);

                        if(squaredSide1 <= 0.4 * cube_len * cube_len)
                        {
                            isInLaserFOV = true;
                        }

                        squaredSide2 = (pointOnYAxis.x - cornerX) * (pointOnYAxis.x - cornerX)
                                + (pointOnYAxis.y - cornerY) * (pointOnYAxis.y - cornerY)
                                + (pointOnYAxis.z - cornerZ) * (pointOnYAxis.z - cornerZ);
                        
                        ang_cos = fabs(squaredSide2 <= 0.5 * cube_len) ? 1.0 :
                            (LIDAR_SP_LEN * LIDAR_SP_LEN + squaredSide1 - squaredSide2) / (2 * LIDAR_SP_LEN * sqrt(squaredSide1));
                        
                        if(ang_cos > 0.5) isInLaserFOV = true;
                    }
                    
                    if (isInLaserFOV)
                    {
                        
                        laserCloudValidInd[laserCloudValidNum] = i + laserCloudWidth * j
                                + laserCloudWidth * laserCloudHeight * k;
                        laserCloudValidNum ++;
                    }
                    laserCloudSurroundInd[laserCloudSurroundNum] = i + laserCloudWidth * j
                            + laserCloudWidth * laserCloudHeight * k;
                    laserCloudSurroundNum ++;
                    
                }
            }
        }
    }

    laserCloudSurfFromMap->clear();
    
    for (int i = 0; i < laserCloudValidNum; i++)
    {
        *laserCloudSurfFromMap += *laserCloudSurfArray[laserCloudValidInd[i]];
    }
}

void feat_points_cbk(const sensor_msgs::PointCloud2::ConstPtr &msg) 
{
  const double timestamp = msg->header.stamp.toSec();
  ROS_DEBUG("get point cloud at time: %.6f", timestamp);

  mtx_buffer.lock();

  if (timestamp < last_timestamp_lidar)
  {
    ROS_ERROR("lidar loop back, clear buffer");
    lidar_buffer.clear();
  }
  last_timestamp_lidar = timestamp;

  lidar_buffer.push_back(msg);

  mtx_buffer.unlock();
  sig_buffer.notify_all();
}

void imu_cbk(const sensor_msgs::Imu::ConstPtr &msg_in) 
{
  sensor_msgs::Imu::Ptr msg(new sensor_msgs::Imu(*msg_in));

  double timestamp = msg->header.stamp.toSec();
  // ROS_DEBUG("get imu at time: %.6f", timestamp);

  mtx_buffer.lock();

  if (timestamp < last_timestamp_imu) {
    ROS_ERROR("imu loop back, clear buffer");
    imu_buffer.clear();
    b_reset = true;
    b_first = true;
  }
  last_timestamp_imu = timestamp;

  imu_buffer.push_back(msg);
  mtx_buffer.unlock();
  sig_buffer.notify_all();
}

void laserCloudSurfLastHandler(const sensor_msgs::PointCloud2ConstPtr& laserCloudSurfLast2)
{
    LaserCloudSurfaceBuff.push_back(*laserCloudSurfLast2);
    timeLaserCloudSurfLast = LaserCloudSurfaceBuff.front().header.stamp.toSec();
}

void StatesHandler(const fast_lio::StatesConstPtr& States)
{
    rot_kp_imu_buff.push_back(*States);
}

bool sync_packages()
{
    if(rot_kp_imu_buff.size() != LaserCloudSurfaceBuff.size())
    {
        return false;
    }
    return true;
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "laserMapping");
    ros::NodeHandle nh;

    ros::Subscriber sub_pcl = nh.subscribe("/laser_cloud_flat", 100, feat_points_cbk);
    ros::Subscriber sub_imu = nh.subscribe("/livox/imu", 100, imu_cbk);
    ros::Subscriber subLaserCloudSurfLast = nh.subscribe<sensor_msgs::PointCloud2>
            ("/livox_undistort", 100, laserCloudSurfLastHandler);
    ros::Publisher pubLaserCloudFullRes = nh.advertise<sensor_msgs::PointCloud2>
            ("/cloud_registered", 100);
    ros::Publisher pubLaserCloudMap = nh.advertise<sensor_msgs::PointCloud2>
            ("/Laser_map", 100);
    // ros::Publisher pubSolvedPose6D = nh.advertise<fast_lio::States>
    //         ("/States_updated", 100);
    ros::Publisher pubOdomAftMapped = nh.advertise<nav_msgs::Odometry> 
            ("/aft_mapped_to_init", 10);
    nav_msgs::Odometry odomAftMapped;


#ifdef DEPLOY
    ros::Publisher mavros_pose_publisher = nh.advertise<geometry_msgs::PoseStamped>("/mavros/vision_pose/pose", 10);
    geometry_msgs::PoseStamped msg_body_pose;
#endif
    std::string map_file_path;
    bool dense_map_en;
    double filter_size_corner_min, filter_size_surf_min, filter_size_map_min;
    ros::param::get("~dense_map_enable",dense_map_en);
    ros::param::get("~map_file_path",map_file_path);
    ros::param::get("~filter_size_corner",filter_size_corner_min);
    ros::param::get("~filter_size_surf",filter_size_surf_min);
    ros::param::get("~filter_size_map",filter_size_map_min);
    ros::param::get("~cube_side_length",cube_len);
    
    PointType pointOri, pointSel, coeff;

    cv::Mat matA1(3, 3, CV_32F, cv::Scalar::all(0));
    cv::Mat matD1(1, 3, CV_32F, cv::Scalar::all(0));
    cv::Mat matV1(3, 3, CV_32F, cv::Scalar::all(0));
    cv::Mat matP(6, 6, CV_32F, cv::Scalar::all(0));

    int effect_feat_num    = 0;
    double aver_time_consu = 0;
    double frame_num = 0;
    double deltaR    = 0.0;
    double deltaT    = 0.0;

    pcl::PointCloud<PointType>::Ptr laserCloudSurfLast(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr laserCloudSurf_down(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr laserCloudOri(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr coeffSel(new pcl::PointCloud<PointType>());
    
    //downsample filter initiallize
    pcl::VoxelGrid<PointType> downSizeFilterSurf;
    pcl::VoxelGrid<PointType> downSizeFilterMap;
    downSizeFilterSurf.setLeafSize(filter_size_surf_min, filter_size_surf_min, filter_size_surf_min);
    downSizeFilterMap.setLeafSize(filter_size_map_min, filter_size_map_min, filter_size_map_min);

    for (int i = 0; i < laserCloudNum; i++)
    {
        laserCloudSurfArray[i].reset(new pcl::PointCloud<PointType>());
    }

    /*** debug record ***/
    std::ofstream fout_pre, fout_out;
    fout_pre.open(DEBUG_FILE_DIR("mat_pre.txt"),std::ios::out);
    fout_out.open(DEBUG_FILE_DIR("mat_out_time.txt"),std::ios::out);
    if (fout_pre && fout_out)
        std::cout << "~~~~"<<ROOT_DIR<<" file opened" << std::endl;
    else
        std::cout << "~~~~"<<ROOT_DIR<<" doesn't exist" << std::endl;
    std::vector<double> T1, s_plot, s_plot2, s_plot3;
    /*** debug record ***/
    
//------------------------------------------------------------------------------------------------------
    ros::Rate rate(500);
    bool status = ros::ok();
    while (status)
    {
        ros::spinOnce();

        while(!LaserCloudSurfaceBuff.empty() && !rot_kp_imu_buff.empty() && sync_packages()) 
        {
            const static double first_lidar_time = LaserCloudSurfaceBuff.front().header.stamp.toSec();
            bool Need_Init = ((timeLaserCloudSurfLast - first_lidar_time) < INIT_TIME) ? true : false;
            if(Need_Init) {std::cout<<"||||||||||Initiallizing LiDar||||||||||"<<std::endl;}
            double t1,t2,t3,t4,match_start, match_time, solve_start, solve_time, pca_time, svd_time;

            match_time = 0;
            solve_time = 0;
            pca_time   = 0;
            svd_time   = 0;

            t1 = omp_get_wtime();
            
            /** Get the propagated states **/
            Eigen::Vector3d gravity, bias_g, bias_a;
            Eigen::Matrix<double, DIM_OF_STATES, DIM_OF_STATES> cov_stat_cur;
            auto states_propagted = rot_kp_imu_buff.front();
            gravity<<VEC_FROM_ARRAY(states_propagted.gravity);
            bias_g<<VEC_FROM_ARRAY(states_propagted.bias_gyr);
            bias_a<<VEC_FROM_ARRAY(states_propagted.bias_acc);
            T_global_cur<<VEC_FROM_ARRAY(states_propagted.pos_end);
            V_global_cur<<VEC_FROM_ARRAY(states_propagted.vel_end);
            R_global_cur=Eigen::Map<Eigen::Matrix3d>(states_propagted.rot_end.data());
            cov_stat_cur=Eigen::Map<Eigen::Matrix<double, DIM_OF_STATES, DIM_OF_STATES> >(states_propagted.cov.data());

            Eigen::Vector3d euler_cur = correct_pi(R_global_cur.eulerAngles(1, 0, 2));
            euler_cur(0)  = euler_cur(0);
            euler_cur(1)  = euler_cur(1);
            euler_cur(2)  = euler_cur(2);
            T_global_cur(0)  = T_global_cur(0);
            T_global_cur(1)  = T_global_cur(1);
            T_global_cur(2)  = T_global_cur(2);

            #ifdef DEBUG_PRINT
            fout_pre << std::setw(10) << timeLaserCloudSurfLast << " " << euler_cur.transpose()*57.3 << " " << T_global_cur.transpose() << " " << V_global_cur.transpose() << std::endl;
            std::cout<<"pre-integrated states: "<<euler_cur.transpose()*57.3<<" "<<T_global_cur.transpose()<<" "<<V_global_cur.transpose()<<" "<<bias_g.transpose()<<" "<<bias_a.transpose()<<std::endl;
            #endif
            
            lasermap_fov_segment();
            
            /*** downsample the laser cloud ***/
            laserCloudSurfLast->clear();
            laserCloudSurf_down->clear();
            pcl::fromROSMsg(LaserCloudSurfaceBuff.front(), *laserCloudSurfLast);

            downSizeFilterSurf.setInputCloud(laserCloudSurfLast);
            downSizeFilterMap.setInputCloud(laserCloudSurfFromMap);
            downSizeFilterSurf.filter(*laserCloudSurf_down);
            downSizeFilterMap.filter(*laserCloudSurfFromMap);

            int laserCloudSurfFromMapNum = laserCloudSurfFromMap->points.size();
            int laserCloudSurf_down_size = laserCloudSurf_down->points.size();
            std::cout<<"[ mapping ]: Raw feature num: "<<laserCloudSurfLast->points.size()<<" Map num: "<<laserCloudSurfFromMapNum<<std::endl;

            pcl::PointCloud<PointType>::Ptr coeffSel_tmpt
                (new pcl::PointCloud<PointType>(*laserCloudSurf_down));
            pcl::PointCloud<PointType>::Ptr laserCloudSurf_down_updated
                (new pcl::PointCloud<PointType>(*laserCloudSurf_down));

            if (laserCloudSurfFromMapNum > 100)
            {
                kdtreeSurfFromMap->setInputCloud(laserCloudSurfFromMap);
                t2 = omp_get_wtime();
                std::vector<bool> point_selected_surf(laserCloudSurf_down_size, true);
                std::vector<std::vector<int>> pointSearchInd_surf(laserCloudSurf_down_size);
                
                int  rematch_num = 0;
                bool rematch_en = 0;
                
                for (iterCount = 0; iterCount < NUM_MAX_ITERATIONS; iterCount++) 
                {
                    match_start = omp_get_wtime();
                    laserCloudOri->clear();
                    coeffSel->clear();
                    /** closest surface search and seclection **/
                    omp_set_num_threads(4);
                    #pragma omp parallel for
                    for (int i = 0; i < laserCloudSurf_down_size; i++)
                    {
                        PointType &pointOri_tmpt = laserCloudSurf_down->points[i];
                        PointType &pointSel_tmpt = laserCloudSurf_down_updated->points[i];
                        pointAssociateToMap(&pointOri_tmpt, &pointSel_tmpt);

                        std::vector<float> pointSearchSqDis_surf;
                        auto &points_near = pointSearchInd_surf[i];
                        if (iterCount == 0 || rematch_en)
                        {
                            /** Rematch (Reprojection) **/
                            kdtreeSurfFromMap->nearestKSearch(pointSel_tmpt, NUM_MATCH_POINTS, points_near, pointSearchSqDis_surf);
                            if (pointSearchSqDis_surf[NUM_MATCH_POINTS - 1] < 5.0)
                            {
                                point_selected_surf[i] = true;
                            }
                        }
                        
                        if (! point_selected_surf[i]) continue;

                        double pca_start = omp_get_wtime();

                        /// using minimum square method
                        cv::Mat matA0(NUM_MATCH_POINTS, 3, CV_32F, cv::Scalar::all(0));
                        cv::Mat matB0(NUM_MATCH_POINTS, 1, CV_32F, cv::Scalar::all(-1));
                        cv::Mat matX0(NUM_MATCH_POINTS, 1, CV_32F, cv::Scalar::all(0));

                        for (int j = 0; j < NUM_MATCH_POINTS; j++)
                        {
                            matA0.at<float>(j, 0) = laserCloudSurfFromMap->points[points_near[j]].x;
                            matA0.at<float>(j, 1) = laserCloudSurfFromMap->points[points_near[j]].y;
                            matA0.at<float>(j, 2) = laserCloudSurfFromMap->points[points_near[j]].z;
                        }

                        //matA0*matX0=matB0
                        //AX+BY+CZ+D = 0 <=> AX+BY+CZ=-D <=> (A/D)X+(B/D)Y+(C/D)Z = -1
                        //(X,Y,Z)<=>mat_a0
                        //A/D, B/D, C/D <=> mat_x0
            
                        cv::solve(matA0, matB0, matX0, cv::DECOMP_QR);  //TODO

                        float pa = matX0.at<float>(0, 0);
                        float pb = matX0.at<float>(1, 0);
                        float pc = matX0.at<float>(2, 0);
                        float pd = 1;

                        //ps is the norm of the plane norm_vec vector
                        //pd is the distance from point to plane
                        float ps = sqrt(pa * pa + pb * pb + pc * pc);
                        pa /= ps;
                        pb /= ps;
                        pc /= ps;
                        pd /= ps;

                        bool planeValid = true;
                        for (int j = 0; j < NUM_MATCH_POINTS; j++)
                        {
                            if (fabs(pa * laserCloudSurfFromMap->points[points_near[j]].x +
                                        pb * laserCloudSurfFromMap->points[points_near[j]].y +
                                        pc * laserCloudSurfFromMap->points[points_near[j]].z + pd) > 0.1)
                            {
                                planeValid = false;
                                point_selected_surf[i] = false;
                                break;
                            }
                        }

                        if (planeValid) 
                        {
                            //loss fuction
                            float pd2 = pa * pointSel_tmpt.x + pb * pointSel_tmpt.y + pc * pointSel_tmpt.z + pd;
                            //if(fabs(pd2) > 0.1) continue;
                            float s = 1 - 0.9 * fabs(pd2) / sqrt(sqrt(pointSel_tmpt.x * pointSel_tmpt.x + pointSel_tmpt.y * pointSel_tmpt.y + pointSel_tmpt.z * pointSel_tmpt.z));

                            if (s > 0.1)
                            {
                                point_selected_surf[i] = true;
                                coeffSel_tmpt->points[i].x = s * pa;
                                coeffSel_tmpt->points[i].y = s * pb;
                                coeffSel_tmpt->points[i].z = s * pc;
                                coeffSel_tmpt->points[i].intensity = s * pd2;
                            }
                            else
                            {
                                point_selected_surf[i] = false;
                            }
                        }

                        pca_time += omp_get_wtime() - pca_start;
                    }

                    double total_residual = 0.0;
                    for (int i = 0; i < coeffSel_tmpt->points.size(); i++)
                    {
                        float error_abs = std::abs(coeffSel_tmpt->points[i].intensity);
                        if (point_selected_surf[i]  && (error_abs < 0.5))
                        {
                            laserCloudOri->push_back(laserCloudSurf_down->points[i]);
                            coeffSel->push_back(coeffSel_tmpt->points[i]);
                            total_residual += error_abs;
                        }
                    }

                    int laserCloudSelNum = laserCloudOri->points.size();
                    double ave_residual = total_residual / laserCloudSelNum;

                    effect_feat_num = coeffSel->size();
                    if(iterCount == 1) std::cout << "[ mapping ]: Effective feature num: "<<effect_feat_num<<std::endl;

                    match_time += omp_get_wtime() - match_start;
                    solve_start = omp_get_wtime();

                    if (laserCloudSelNum < 50) {
                        continue;
                    }
                    
                    /*** Measuremnt Jacobian matrix: H and measurents vector: meas_vec ***/
                    Eigen::MatrixXd H(laserCloudSelNum, DIM_OF_STATES) ;
                    Eigen::VectorXd meas_vec(laserCloudSelNum);

                    omp_set_num_threads(4);
                    #pragma omp parallel for
                    for (int i = 0; i < laserCloudSelNum; i++)
                    {
                        const PointType &laser_p  = laserCloudOri->points[i];
                        Eigen::Vector3d point_this(laser_p.x, laser_p.y, laser_p.z);
                        Eigen::Matrix3d point_crossmat;
                        point_crossmat<<SKEW_SYM_MATRX(point_this);

                        /*** get the normal vector of closest surface/corner ***/
                        const PointType &norm_p = coeffSel->points[i];
                        Eigen::Vector3d norm_vec(norm_p.x, norm_p.y, norm_p.z);

                        /*** calculate the Measuremnt Jacobian matrix H ***/
                        Eigen::Vector3d A(point_crossmat * R_global_cur.transpose() * norm_vec);
                        H.row(i) = Eigen::Matrix<double, 1, DIM_OF_STATES>::Zero();
                        H.block<1,6>(i,0) << VEC_FROM_ARRAY(A), norm_p.x, norm_p.y, norm_p.z;

                        /*** Measuremnt: distance to the closest surface/corner ***/
                        meas_vec(i) = - norm_p.intensity;
                    }

                    Eigen::Vector3d rot_add, t_add, v_add, bg_add, ba_add, g_add;
                    Eigen::VectorXd solution(DIM_OF_STATES);
                    Eigen::MatrixXd K(DIM_OF_STATES, DIM_OF_STATES);

                    if (Need_Init)
                    {
                        /*** Iterative Kalan Filter (initiallization) ***/
                        Eigen::MatrixXd H_init(Eigen::Matrix<double, 9, DIM_OF_STATES>::Zero());
                        Eigen::MatrixXd z_init(Eigen::Matrix<double, 9, 1>::Zero());
                        H_init.block<3,3>(0,0)  = Eigen::Matrix3d::Identity();
                        H_init.block<3,3>(3,3)  = Eigen::Matrix3d::Identity();
                        H_init.block<3,3>(6,15) = Eigen::Matrix3d::Identity();
                        z_init.block<3,1>(0,0)  = - Log(R_global_cur);
                        z_init.block<3,1>(0,0)  = - T_global_cur;

                        auto H_init_T = H_init.transpose();
                        auto &&K_init = cov_stat_cur * H_init_T * (H_init * cov_stat_cur * H_init_T + 0.0001 * Eigen::Matrix<double,9,9>::Identity()).inverse();
                        solution      = K_init * z_init;
                        for (int ind = 0; ind < 3; ind ++)
                        {
                            rot_add[ind] = solution(ind);
                            t_add[ind]   = solution(ind+3);
                            v_add[ind]   = solution(ind+6);
                            bg_add[ind]  = solution(ind+9);
                            ba_add[ind]  = solution(ind+12);
                            g_add[ind]   = solution(ind+15);
                        }

                        R_global_cur.setIdentity();
                        T_global_cur.setZero();
                        V_global_cur.setZero();
                        bias_g      += bg_add;
                        bias_a      += ba_add;
                        cov_stat_cur = (Eigen::MatrixXd::Identity(DIM_OF_STATES, DIM_OF_STATES) - K_init * H_init) * cov_stat_cur;
                    }
                    else
                    {
                        /*** Iterative Kalman Filter Update ***/
                        auto &&H_T = H.transpose();
                        Eigen::Matrix<double, DIM_OF_STATES, DIM_OF_STATES> &&K_1 = (H_T * H + (cov_stat_cur / LASER_POINT_COV).inverse()).inverse();
                        K = K_1 * H_T;
                        solution = K * meas_vec;
                        // std::cout<<"***solution: "<<solution.transpose()<<std::endl;
                        for (int ind = 0; ind < 3; ind ++)
                        {
                            rot_add[ind] = solution(ind);
                            t_add[ind]   = solution(ind+3);
                            v_add[ind]   = solution(ind+6);
                            bg_add[ind]  = solution(ind+9);
                            ba_add[ind]  = solution(ind+12);
                            g_add[ind]   = solution(ind+15);
                        }

                        R_global_cur = R_global_cur * Exp(rot_add);
                        T_global_cur = T_global_cur + t_add;
                        V_global_cur = V_global_cur + v_add;
                        bias_g  += bg_add;
                        bias_a  += ba_add;
                        gravity += g_add;

                        deltaR = rot_add.norm() * 57.3;
                        deltaT = t_add.norm() * 100.0;
                    }

                    euler_cur = correct_pi(R_global_cur.eulerAngles(1, 0, 2));

                    #ifdef DEBUG_PRINT
                    std::cout<<"***new stat: "<<euler_cur.transpose()*57.3<<" p "<<T_global_cur.transpose()<<" v "<<V_global_cur.transpose()<<" ba "<<bias_a.transpose()<<" G "<<gravity.transpose()<<std::endl;
                    std::cout<<"dR & dT: "<<deltaR<<" "<<deltaT<<std::endl;
                    #endif

                    rematch_en = false;

                    /*** Rematch Judgement ***/
                    if ((deltaR < 0.015 && deltaT < 0.015))
                    {
                        rematch_en = true;
                        rematch_num ++;
                    }

                    /*** End Judgements and Covariance Update ***/
                    if (rematch_num >= 2)
                    {
                        if (!Need_Init)
                        {
                            /*** Covariance Update ***/
                            cov_stat_cur = (Eigen::MatrixXd::Identity(DIM_OF_STATES, DIM_OF_STATES) - K * H) * cov_stat_cur;
                        }
                        solve_time += omp_get_wtime() - solve_start;
                        break;
                    }
                    solve_time += omp_get_wtime() - solve_start;
                }

                std::cout<<"[ mapping ]: iteration count: "<<iterCount+1<<std::endl;
            }

            LaserCloudSurfaceBuff.pop_front();
            rot_kp_imu_buff.pop_front();

            /**** Regester new feature point to map ***/
            if (!LaserCloudSurfaceBuff.empty() && !rot_kp_imu_buff.empty())
            {
                timeLaserCloudSurfLast = LaserCloudSurfaceBuff.front().header.stamp.toSec();
            }
            
            bool if_cube_updated[laserCloudNum] = {0};
            for (int i = 0; i < laserCloudSurf_down_size; i++)
            {
                PointType &pointSel = laserCloudSurf_down_updated->points[i];

                int cubeI = int((pointSel.x + 0.5 * cube_len) / cube_len) + laserCloudCenWidth;
                int cubeJ = int((pointSel.y + 0.5 * cube_len) / cube_len) + laserCloudCenHeight;
                int cubeK = int((pointSel.z + 0.5 * cube_len) / cube_len) + laserCloudCenDepth;

                if (pointSel.x + 0.5 * cube_len < 0) cubeI--;
                if (pointSel.y + 0.5 * cube_len < 0) cubeJ--;
                if (pointSel.z + 0.5 * cube_len < 0) cubeK--;

                if (cubeI >= 0 && cubeI < laserCloudWidth &&
                        cubeJ >= 0 && cubeJ < laserCloudHeight &&
                        cubeK >= 0 && cubeK < laserCloudDepth) {
                    int cubeInd = cubeI + laserCloudWidth * cubeJ + laserCloudWidth * laserCloudHeight * cubeK;
                    laserCloudSurfArray[cubeInd]->push_back(pointSel);
                    if_cube_updated[cubeInd] = true;
                }
            }
            for (int i = 0; i < laserCloudValidNum; i++)
            {
                int ind = laserCloudValidInd[i];

                if(if_cube_updated[ind])
                {
                    downSizeFilterSurf.setInputCloud(laserCloudSurfArray[ind]);
                    downSizeFilterSurf.filter(*laserCloudSurfArray[ind]);
                }

            }
            t3 = omp_get_wtime();

            /******* Publish Lidar states *******/
            fast_lio::States Pose6D_Solved;
            save_states(Pose6D_Solved, gravity, bias_g, bias_a, \
                        T_global_cur, V_global_cur, R_global_cur, cov_stat_cur); //std::cout<<"!!!! Sent pose:"<<T_global_cur.transpose()<<std::endl;
            // Pose6D_Solved.IMUpose.clear();
            pubSolvedPose6D.publish(Pose6D_Solved);

            /******* Publish current frame points in world coordinates:  *******/            
            laserCloudFullRes2->clear();
            *laserCloudFullRes2 = dense_map_en ? (*laserCloudSurfLast) : (* laserCloudSurf_down);
            // *laserCloudFullRes2 = dense_map_en ? (*laserCloudFullRes) : (* laserCloudSurf_down);

            int laserCloudFullResNum = laserCloudFullRes2->points.size();

            pcl::PointXYZRGB temp_point;
            laserCloudFullResColor->clear();
            for (int i = 0; i < laserCloudFullResNum; i++)
            {
                RGBpointAssociateToMap(&laserCloudFullRes2->points[i], &temp_point);
                laserCloudFullResColor->push_back(temp_point);
            }

            sensor_msgs::PointCloud2 laserCloudFullRes3;
            pcl::toROSMsg(*laserCloudFullResColor, laserCloudFullRes3);
            laserCloudFullRes3.header.stamp = ros::Time().fromSec(timeLaserCloudSurfLast);
            laserCloudFullRes3.header.frame_id = "/camera_init";
            pubLaserCloudFullRes.publish(laserCloudFullRes3);

            /******* Publish Maps:  *******/
            sensor_msgs::PointCloud2 laserCloudMap;
            pcl::toROSMsg(*laserCloudSurfFromMap, laserCloudMap);
            laserCloudMap.header.stamp = ros::Time::now();//ros::Time().fromSec(timeLaserCloudSurfLast);
            laserCloudMap.header.frame_id = "/camera_init";
            pubLaserCloudMap.publish(laserCloudMap);

            /******* Publish Odometry ******/
            geometry_msgs::Quaternion geoQuat = tf::createQuaternionMsgFromRollPitchYaw
                    (euler_cur(2), - euler_cur(0), - euler_cur(1));
            odomAftMapped.header.frame_id = "/camera_init";
            odomAftMapped.child_frame_id = "/aft_mapped";
            odomAftMapped.header.stamp = ros::Time::now();//ros::Time().fromSec(timeLaserCloudSurfLast);
            odomAftMapped.pose.pose.orientation.x = -geoQuat.y;
            odomAftMapped.pose.pose.orientation.y = -geoQuat.z;
            odomAftMapped.pose.pose.orientation.z = geoQuat.x;
            odomAftMapped.pose.pose.orientation.w = geoQuat.w;
            odomAftMapped.pose.pose.position.x = T_global_cur(0);
            odomAftMapped.pose.pose.position.y = T_global_cur(1);
            odomAftMapped.pose.pose.position.z = T_global_cur(2);

            pubOdomAftMapped.publish(odomAftMapped);

            static tf::TransformBroadcaster br;
            tf::Transform                   transform;
            tf::Quaternion                  q;
            transform.setOrigin( tf::Vector3( odomAftMapped.pose.pose.position.x,
                                                odomAftMapped.pose.pose.position.y,
                                                odomAftMapped.pose.pose.position.z ) );
            q.setW( odomAftMapped.pose.pose.orientation.w );
            q.setX( odomAftMapped.pose.pose.orientation.x );
            q.setY( odomAftMapped.pose.pose.orientation.y );
            q.setZ( odomAftMapped.pose.pose.orientation.z );
            transform.setRotation( q );
            br.sendTransform( tf::StampedTransform( transform, odomAftMapped.header.stamp, "/camera_init", "/aft_mapped" ) );

#ifdef DEPLOY
            msg_body_pose.header.stamp = ros::Time::now();
            msg_body_pose.header.frame_id = "/camera_odom_frame";
            msg_body_pose.pose.position.x = T_global_cur(0);
            msg_body_pose.pose.position.y = - T_global_cur(1);
            msg_body_pose.pose.position.z = - T_global_cur(2);
            msg_body_pose.pose.orientation.x = - geoQuat.y;
            msg_body_pose.pose.orientation.y = - geoQuat.z;
            msg_body_pose.pose.orientation.z = geoQuat.x;
            msg_body_pose.pose.orientation.w = geoQuat.w;
            mavros_pose_publisher.publish(msg_body_pose);
#endif

            /*** save debug variables ***/
            t4 = omp_get_wtime();
            frame_num ++;
            aver_time_consu = aver_time_consu * (frame_num - 1) / frame_num + (t3 - t1) / frame_num;
            // aver_time_consu = aver_time_consu * 0.5 + (t4 - t1) * 0.5;
            T1.push_back(timeLaserCloudSurfLast);
            s_plot.push_back(aver_time_consu);
            s_plot2.push_back(double(deltaR));
            s_plot3.push_back(double(deltaT));

            std::cout<<"[ mapping ]: time: selection "<<t2-t1 <<" match "<<match_time<<" solve: "<<solve_time<<" total: "<<t3 - t1<<std::endl;
            fout_out << std::setw(10) << timeLaserCloudSurfLast << " " << t3-t1 << " " << effect_feat_num << std::endl;
        }
        status = ros::ok();
        rate.sleep();
    }
    //--------------------------save map---------------
    std::string surf_filename(map_file_path + "/surf.pcd");
    std::string corner_filename(map_file_path + "/corner.pcd");
    std::string all_points_filename(map_file_path + "/all_points.pcd");

    pcl::PointCloud<PointType> surf_points, corner_points;
    surf_points = *laserCloudSurfFromMap;
    fout_out.close();
    fout_pre.close();
    if (surf_points.size() > 0 && corner_points.size() > 0) 
    {
    pcl::PCDWriter pcd_writer;
    std::cout << "saving...";
    pcd_writer.writeBinary(surf_filename, surf_points);
    pcd_writer.writeBinary(corner_filename, corner_points);
    }
    else
    {
        // #ifdef DEBUG_PRINT
        #ifndef DEPLOY
        if (!T1.empty())
        {
            plt::named_plot("time consumed",T1,s_plot);
            // plt::named_plot("R_residual",T1,s_plot2);
            // plt::named_plot("T_residual",T1,s_plot3);
            plt::legend();
            plt::show();
            plt::pause(0.5);
            plt::close();
            // plt::save("/home/xw/catkin_like_loam/src/LIEK_LOAM/a.png");
        }
        std::cout << "no points saved";
        // #endif
        #endif
    }
    //--------------------------
    //  loss_output.close();
  return 0;
}
