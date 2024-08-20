
#include "laserProcessingClass.h"
#include "orbextractor.h"
#include <opencv2/highgui.hpp>
#include <opencv2/opencv.hpp>
#include <sensor_msgs/Image.h>
#include <cv_bridge/cv_bridge.h>
#include <Eigen/Dense>
#include <unordered_set> // 添加這行頭文件  `

//多threads
#include <thread>
#include <vector>
#include <mutex>

#include "opencv2/img_hash.hpp"



void LaserProcessingClass::init(lidar::Lidar lidar_param_in){
    lidar_param = lidar_param_in;
}


void LaserProcessingClass::downSamplingToMap(const pcl::PointCloud<pcl::PointXYZI>::Ptr& surf_pc_in, pcl::PointCloud<pcl::PointXYZI>::Ptr& surf_pc_out){
    downSizeFilterSurf.setInputCloud(surf_pc_in);
    downSizeFilterSurf.filter(*surf_pc_out);
 
}

//surface===============================

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <opencv2/opencv.hpp>
#include <thread>
#include <vector>
#include <cmath>

void processPointCloudRegions_surface(const pcl::PointCloud<pcl::PointXYZI>::Ptr& surf_first, 
                                      int startIdx, int endIdx, 
                                      int half_window_size, double intensity_threshold, 
                                      double gradient_threshold, int window_size, 
                                      std::vector<pcl::PointXYZI>& planePixels) {
    for (int i = startIdx + half_window_size; i < endIdx - half_window_size; i = i + 4 ) {
        bool isPlanar = false;
        
        // 1. 检查 x 轴上的最大最小强度值是否在阈值内
        float minIntensity = std::numeric_limits<float>::max();
        float maxIntensity = std::numeric_limits<float>::min();

        for (int wx = -half_window_size; wx <= half_window_size; ++wx) {
            float intensity = surf_first->points[i + wx].intensity;
            if (!std::isnan(intensity)) {
                minIntensity = std::min(minIntensity, intensity);
                maxIntensity = std::max(maxIntensity, intensity);
            }
        }

        if (maxIntensity - minIntensity <= intensity_threshold) {
            // 如果强度值在阈值内，则检查 y 轴上的梯度
            float depth_top = surf_first->points[i + half_window_size].z;
            float depth_center = surf_first->points[i].z;
            float depth_bottom = surf_first->points[i - half_window_size].z;

            double gradient_1 = std::abs(depth_top - depth_center);
            double gradient_2 = std::abs(depth_bottom - depth_center);

            double gradient = std::abs(gradient_1 - gradient_2);

            if (gradient <= gradient_threshold) {
                isPlanar = true;
            }
        }

        if (!isPlanar) {
            // 2. 检查 y 轴上的最大最小强度值是否在阈值内
            float minIntensity = std::numeric_limits<float>::max();
            float maxIntensity = std::numeric_limits<float>::min();

            for (int wy = -half_window_size; wy <= half_window_size; ++wy) {
                float intensity = surf_first->points[i + wy].intensity;
                if (!std::isnan(intensity)) {
                    minIntensity = std::min(minIntensity, intensity);
                    maxIntensity = std::max(maxIntensity, intensity);
                }
            }

            if (maxIntensity - minIntensity <= intensity_threshold) {
                // 如果强度值在阈值内，则检查 x 轴上的梯度
                float depth_left = surf_first->points[i + half_window_size].z;
                float depth_center = surf_first->points[i].z;
                float depth_right = surf_first->points[i - half_window_size].z;

                double gradient_1 = std::abs(depth_left - depth_center);
                double gradient_2 = std::abs(depth_right - depth_center);

                double gradient = std::abs(gradient_1 - gradient_2);

                if (gradient <= gradient_threshold) {
                    isPlanar = true;
                }
            }
        }

        if (isPlanar) {
            planePixels.push_back(surf_first->points[i]);
        }
    }
}


void processPointCloud_surface(const pcl::PointCloud<pcl::PointXYZI>::Ptr& surf_first,
                               int half_window_size, double depth_threshold, 
                               double gradient_threshold, int window_size, 
                               std::vector<pcl::PointXYZI>& planePixels) {
    int numThreads = 64; // Number of threads to use
    std::vector<std::thread> threads;
    std::vector<std::vector<pcl::PointXYZI>> planePixelsList(numThreads);

    // Divide the point cloud into regions and assign threads
    int totalPoints = surf_first->points.size();
    int pointsPerThread = totalPoints / numThreads;
    int remainingPoints = totalPoints % numThreads;
    int startIdx = 0;

    for (int i = 0; i < numThreads; ++i) {
        int endIdx = startIdx + pointsPerThread;
        if (i == numThreads - 1) endIdx += remainingPoints;
        threads.emplace_back(processPointCloudRegions_surface, surf_first, startIdx, endIdx, 
                             half_window_size, depth_threshold, gradient_threshold, window_size, 
                             std::ref(planePixelsList[i]));
        startIdx = endIdx;
    }

    // Join threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Combine results from each thread
    for (const auto& pixels : planePixelsList) {
        planePixels.insert(planePixels.end(), pixels.begin(), pixels.end());
    }
}


//=============edge=================================

void LaserProcessingClass::featureExtractionWithSobel(pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_in,
                                                      pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_out_edge,
                                                      pcl::PointCloud<pcl::PointXYZI>::Ptr& surf_first) {
    // 设定Sobel算子
    cv::Mat sobelX = (cv::Mat_<int>(3, 3) << -1, 0, 1, 
                                             -2, 0, 2, 
                                             -1, 0, 1);
    cv::Mat sobelY = (cv::Mat_<int>(3, 3) << -1, -2, -1, 
                                              0, 0, 0, 
                                              1, 2, 1);

    // 创建一个二维矩阵用于存储每条扫描线的投影点
    cv::Mat projection_image(pc_in->points.size(), 2, CV_64F);

    // 将3D点云投影到2D平面（xy平面）
    for (size_t i = 0; i < pc_in->points.size(); i++) {
        projection_image.at<double>(i, 0) = pc_in->points[i].x;
        projection_image.at<double>(i, 1) = pc_in->points[i].y;
    }

    // 计算Sobel梯度
    cv::Mat grad_x, grad_y, grad_magnitude;
    cv::filter2D(projection_image.col(0), grad_x, -1, sobelX); // 对X轴方向应用Sobel
    cv::filter2D(projection_image.col(1), grad_y, -1, sobelY); // 对Y轴方向应用Sobel

    // 计算梯度幅值
    cv::magnitude(grad_x, grad_y, grad_magnitude);

    // 根据梯度幅值选择边缘点和面特征点
    for (int i = 0; i < grad_magnitude.rows; i = i + 4 ) {
        double grad_value = grad_magnitude.at<double>(i, 0);

        if (grad_value > 0.5) {  // 根据需要调整阈值
            pc_out_edge->push_back(pc_in->points[i]);
        } else {
            surf_first->push_back(pc_in->points[i]);
        }
    }
}

//=========================================
///
void LaserProcessingClass::pointcloudtodepth(pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_in,
                                             sensor_msgs::ImageConstPtr& image_msg, 
                                             Eigen::Matrix<double, 3, 4>& matrix_3Dto2D,
                                             Eigen::Matrix3d& result,
                                             Eigen::Matrix3d& RR,
                                             Eigen::Vector3d& tt,
                                             pcl::PointCloud<pcl::PointXYZI>::Ptr& surf_first,
                                             pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_out_surf
                                             ) {

    // Processing 360-degree point cloud to extract planar features
    int window_size = 5;
    int half_window_size = window_size / 2;
    double depth_threshold = 0.3;
    double gradient_threshold = 0.1;

    std::vector<pcl::PointXYZI> planePixels;

    processPointCloud_surface(surf_first, half_window_size, depth_threshold, gradient_threshold, window_size, planePixels);
  
    pc_out_surf->points.insert(pc_out_surf->points.end(), planePixels.begin(), planePixels.end());
    
    std::cout << "after plane number = " << pc_out_surf->points.size() << std::endl;
}

void LaserProcessingClass::featureExtraction(pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_in, 
                                             pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_out_edge, 
                                             pcl::PointCloud<pcl::PointXYZI>::Ptr& surf_first,
                                             sensor_msgs::ImageConstPtr& image_msg, 
                                             Eigen::Matrix<double, 3, 4>& matrix_3Dto2D){

    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(*pc_in, indices);
    // std::cout << "pcin number = " << (int)pc_in->points.size() << std::endl;

    int N_SCANS = lidar_param.num_lines;
    std::vector<pcl::PointCloud<pcl::PointXYZI>::Ptr> laserCloudScans;
    for(int i=0;i<N_SCANS;i++){
        laserCloudScans.push_back(pcl::PointCloud<pcl::PointXYZI>::Ptr(new pcl::PointCloud<pcl::PointXYZI>()));
    }

    for (int i = 0; i < (int) pc_in->points.size(); i++)
    {
        // if(pc_in->points[i].x >= 0){
            int scanID=0;
            double distance = sqrt(pc_in->points[i].x * pc_in->points[i].x + pc_in->points[i].y * pc_in->points[i].y);
            if(distance<lidar_param.min_distance || distance>lidar_param.max_distance)
                continue;
            double angle = atan(pc_in->points[i].z / distance) * 180 / M_PI;
            
            // if (N_SCANS == 16)
            // {
            //     scanID = int((angle + 15) / 2 + 0.5);
            //     if (scanID > (N_SCANS - 1) || scanID < 0)
            //     {
            //         continue;
            //     }
            // }
            // else if (N_SCANS == 32)
            // {
            //     scanID = int((angle + 92.0/3.0) * 3.0 / 4.0);
            //     if (scanID > (N_SCANS - 1) || scanID < 0)
            //     {
            //         continue;
            //     }
            // }
            // else if (N_SCANS == 64)
            // {   
                if (angle >= -8.83)
                    scanID = int((2 - angle) * 3.0 + 0.5);
                else
                    scanID = N_SCANS / 2 + int((-8.83 - angle) * 2.0 + 0.5);

                if (angle > 2 || angle < -24.33 || scanID > 63 || scanID < 0)
                {
                    continue;
                }
            // }
            // else
            // {
            //     printf("wrong scan number\n");
            // } // cv::imshow("after", gray);
    // cv::wa
            laserCloudScans[scanID]->push_back(pc_in->points[i]); 
        // }
    }

    
featureExtractionWithSobel(pc_in,pc_out_edge,surf_first);


    // // #pragma omp parallel for
    // for (int i = 0; i < (int)edge_first->points.size(); i++) {
    //     if (edge_first->points[i].x >= 0) {
            
    //         Eigen::Vector4d curr_point(edge_first->points[i].x, edge_first->points[i].y, edge_first->points[i].z, 1);
    //         Eigen::Vector3d curr_point_image = matrix_3Dto2D * curr_point;

    //         curr_point_image.x() = curr_point_image.x() / curr_point_image.z();
    //         curr_point_image.y() = curr_point_image.y() / curr_point_image.z();

    //         // 检查投影点是否在边缘上
    //         int x = static_cast<int>(curr_point_image.x());
    //         int y = static_cast<int>(curr_point_image.y());
                
    //         if (x >= 0 && x < gray.cols && y >= 0 && y < gray.rows) {
    //             // if (gray.at<uchar>(y, x) > 0) {
    //                 // 检查周围像素是否部份是边缘 //0717
    //                 bool is_edge_nearby = false;
    //                 int half_window_size = window_size / 2;
    //                 for (int dy = -half_window_size; dy <= half_window_size; dy++) {
    //                     for (int dx = -half_window_size; dx <= half_window_size; dx++) {
    //                         int nx = x + dx;
    //                         int ny = y + dy;
    //                         if (nx >= 0 && nx < gray.cols && ny >= 0 && ny < gray.rows) { //v4
    //                             if (gray.at<uchar>(ny, nx)  > gray.at<uchar>(y, x) + 31  ) {
    //                                 is_edge_nearby = true ;
    //                                 break;
    //                             }
    //                         }
    //                     }
    //                     if (is_edge_nearby) {
    //                         break;
    //                     }
    //                 }

    //                 if (is_edge_nearby) {
    //                     cv::circle(cv_ptr_2->image, cv::Point(x, y), 1, cv::Scalar(0, 0, 255), -1);
    //                     // #pragma omp critical
    //                     {
    //                         pc_out_edge->push_back(edge_first->points[i]);
    //                     }
    //                     // number++;
    //                 }
    //             // }
    //         }
    //     }
    // }
    double map_resolution = 0.3;
    downSizeFilterSurf.setLeafSize(map_resolution * 2, map_resolution * 2, map_resolution * 2);
    // downSamplingToMap(pc_out_edge, pc_out_edge);//0529
    std::cout << "after edge number = " << (int)pc_out_edge->points.size() << std::endl;
    
    // cv::imshow("after edge", cv_ptr_2->image);
    // cv::waitKey(0);

}


// void LaserProcessingClass::featureExtractionFromSector(const pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_in, 
//                                                              std::vector<Double2d>& cloudCurvature, 
//                                                              pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_out_edge ,
//                                                              pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_out_surf
//                                                              ){

//     std::sort(cloudCurvature.begin(), cloudCurvature.end(), [](const Double2d & a, const Double2d & b)
//     { 
//         return a.value < b.value; 
//     }); //由小排到大


//     int largestPickedNum = 0;
//     std::vector<int> picked_points;
//     int point_info_count =0;
//     for (int i = cloudCurvature.size()-1; i >= 0; i=i-1)
//     {
//         int ind = cloudCurvature[i].id; 
//         //檢查該點是否已經存在picked_points裡面 若不存在則執行if裡面的事
//         if(std::find(picked_points.begin(), picked_points.end(), ind)==picked_points.end()){

//             if(cloudCurvature[i].value > 0.06){
//                 largestPickedNum++;
//                 picked_points.push_back(ind);
//             }

//             //一個segment曲率前20大的都push進edge點 //0529
//             if (largestPickedNum <= 20 && cloudCurvature[i].value > 0.06 && std::abs(pc_in->points[ind].y) >= 0.5){
//                 pc_out_edge->push_back(pc_in->points[ind]);
//                 point_info_count++;
//             }
//             //若超過20個之後的點曲率還是大於0.1也是push進edge點  //0529
//             else if(cloudCurvature[i].value > 0.3 && std::abs(pc_in->points[ind].y) >= 0.5){
//                 pc_out_edge->push_back(pc_in->points[ind]);
//                 picked_points.push_back(ind);
//             }
//             else{
//                 pc_out_surf->push_back(pc_in->points[ind]);
//             }

//             // if(std::abs(pc_in->points[i].y) >= 0.5)

//         }
//     }
// }
LaserProcessingClass::LaserProcessingClass(){
    
}

Double2d::Double2d(int id_in, double value_in){
    id = id_in;
    value =value_in;
};

PointsInfo::PointsInfo(int layer_in, double time_in){
    layer = layer_in;
    time = time_in;
};