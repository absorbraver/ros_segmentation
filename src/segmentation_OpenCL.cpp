#include<vector>

//branch
#include <ros/ros.h>
#include <my_pcl_tutorial/segmentation_OpenCL.h>

// OpenCV specific includes
#include <sensor_msgs/Image.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/highgui/highgui.hpp>

// PCL specific includes
#include <sensor_msgs/PointCloud2.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/conversions.h>

using namespace cv;

#define fx 520.0
#define fy 520.0
#define cx 319.5
#define cy 239.5
#define cl_rows 480
#define cl_cols 640
#define cl_imgSize cl_rows*cl_cols

cl_device_id    		        deviceID;
cl_context                  context = NULL;
cl_command_queue            command_queue = NULL;
cl_program                  program = NULL;
cl_kernel                   kernel = NULL;
cl_int                      ret;
cl_mem                      gpu_depth, gpu_normals;
float                       *cpu_depth;
cl_float3                   *cpu_normals;

ros::Publisher pub;
pcl::PointCloud<pcl::PointXYZ>::Ptr point_cloudPtr (new pcl::PointCloud<pcl::PointXYZ>);
cv::Mat normals_image, depth_image, segmented_image, img;
pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_;
int cluster_Index; int neighBor = 10; int timePrint = 6; bool noData;

void GPUMemAlloc(cl_context context, cl_mem* cl_depth, cl_mem* cl_normals)
{
    cl_int         ret;
    cl_mem         depth, normals;

    depth = clCreateBuffer(context, CL_MEM_READ_WRITE, cl_imgSize * sizeof(float), NULL, &ret);
    CheckCLError (ret, "Could not create clCreateBuffer d_X.", "Created clCreateBuffer d_X.");

    normals = clCreateBuffer(context, CL_MEM_READ_WRITE, cl_imgSize * sizeof(cl_float3), NULL, &ret);
    CheckCLError (ret, "Could not create clCreateBuffer d_X.", "Created clCreateBuffer d_X.");

    cpu_depth = (float*)malloc(cl_imgSize * sizeof(float));
    cpu_normals = (cl_float3*)malloc(cl_imgSize * sizeof(cl_float3));

    *cl_depth = depth; *cl_normals = normals;
}

void copyDepthToCPU()
{
   int loop = 0;
   noData = true;
   for(int row=0; row < depth_image.rows; row++)
    {
       for(int col=0; col < depth_image.cols; col++)       
        {
          if(isnan(depth_image.at<float>(row, col)))
          {
            cpu_depth[loop] = 0; loop++; continue;
          }
          cpu_depth[loop] = depth_image.at<float>(row, col); loop++;
          noData = false;        
        }
    }
}

void copyDepthToGPU(cl_command_queue command_queue, cl_mem* gpu_depth)
{
    cl_int  ret;
    ret = clEnqueueWriteBuffer(command_queue, *gpu_depth, CL_TRUE, 0, cl_imgSize * sizeof(float), cpu_depth, 0, NULL, NULL);
    CheckCLError (ret, "Could not create clEnqueueWriteBuffer.", "Created clEnqueueWriteBuffer.");	
}

void OpenCL_start()
{
   InitializeOpenCL(&deviceID, &context, &command_queue, &program);
   GPUMemAlloc(context, &gpu_depth, &gpu_normals);
}

void copyNormalsFromGPU(cl_command_queue command_queue, cl_mem gpu_normals)
{
  cv::Mat normals(depth_image.size(), CV_32FC3);
  normals_image = normals.clone();
   cl_int  ret;
   ret = clEnqueueReadBuffer(command_queue, gpu_normals, CL_TRUE, 0, cl_imgSize * sizeof(cl_float3), cpu_normals, 0, NULL, NULL);
   CheckCLError (ret, "Could not create clEnqueueReadBuffer.", "Created clEnqueueReadBuffer.");
   for(int i = 0; i < cl_imgSize; i++)
   {
     int row = i / cl_cols; int col = i % cl_cols;
     if(cpu_normals[i].x == 0 & cpu_normals[i].y == 0 & cpu_normals[i].z == 0) 
     {
       Vec3f d(0, 0, 0); normals_image.at<Vec3f>(row, col) = d; continue;
     }
     float dzdx = cpu_normals[i].x;
     float dzdy = cpu_normals[i].y;
     Vec3f d(-dzdx, -dzdy, 0.001f);
     Vec3f n; normalize(d, n, 1.0, 0.0, NORM_L2);
     normals_image.at<Vec3f>(row, col) = n;
   }
}

void OpenCL_process()
{
  clock_t start, end;
  double time_used;

  copyDepthToCPU();
  if(noData) return;

  start = clock();
  copyDepthToGPU(command_queue, &gpu_depth);
  end = clock();
  time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
  if(timePrint) std::cerr << "Time to execute copyDepthToGPU: " << time_used << "\n";
  
  // Create OpenCL Kernel
  kernel = clCreateKernel(program, "GPU_NormalCompute", &ret);
  CheckCLError (ret, "Could not create clCreateKernel.", "Created clCreateKernel.");
  // Set OpenCL Kernel Parameters
  ret = clSetKernelArg(kernel, 0, sizeof(cl_mem), (void *)&gpu_depth);
  ret = clSetKernelArg(kernel, 1, sizeof(cl_mem), (void *)&gpu_normals);
  
  // Execute OpenCL Kernel
  start = clock();
  size_t * global = (size_t*) malloc(sizeof(size_t)*2); global[0] = cl_rows; global[1] = cl_cols;
  ret = clEnqueueNDRangeKernel(command_queue, kernel, 2, NULL, global, NULL, 0, NULL, NULL);
  end = clock();
  time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
  if(timePrint) std::cerr << "Time to execute kernel : " << time_used << "\n";

  start = clock();
  copyNormalsFromGPU(command_queue, gpu_normals);
  end = clock();
  time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
  if(timePrint) std::cerr << "Time to execute copyNormalsFromGPU: " << time_used << "\n";
}

void depthToClould()
{
   cloud_.reset(new pcl::PointCloud<pcl::PointXYZRGB>);
   pcl::PointXYZRGB point;
   for(int row=0; row < depth_image.rows; row++)
    {
       for(int col=0; col < depth_image.cols; col++)       
        {
          if(isnan(depth_image.at<float>(row, col))) continue;
          double depth = depth_image.at<float>(row, col);
          //if(col%10 == 0 & row%10 == 0) std::cerr << "depth: " <<  depth << "\n";
          point.x = (col-cx) * depth / fx;
          point.y = (row-cy) * depth / fy;
          point.z = depth;

           if (segmented_image.at<float>(row, col) == -1) continue; 
           else if (segmented_image.at<float>(row, col) == -1) //Red	#FF0000	(255,0,0)
			     {
				      point.r = 0;
				      point.g = 0;
				      point.b = 255;
			     }
			    else if (segmented_image.at<float>(row, col) == 1) //Lime	#00FF00	(0,255,0)
			     {
				      point.r = 255;
				      point.g = 255;
				      point.b = 255;
			     }
			    else if (segmented_image.at<float>(row, col) == 2) // Blue	#0000FF	(0,0,255)
			     {
				      point.r = 255;
				      point.g = 0;
				      point.b = 0;
			     }
			    else if (segmented_image.at<float>(row, col) == 3) // Yellow	#FFFF00	(255,255,0)
			     {
				      point.r = 255;
				      point.g = 255;
				      point.b = 0;
			     }
			    else if (segmented_image.at<float>(row, col) == 4) //Cyan	#00FFFF	(0,255,255)
			     {
				      point.r = 0;
				      point.g = 255;
				      point.b = 255;
			     }
			    else if (segmented_image.at<float>(row, col) == 5) // Magenta	#FF00FF	(255,0,255)
			     {
				      point.r = 255;
				      point.g = 0;
				      point.b = 255;
			     }
			    else if (segmented_image.at<float>(row, col) == 6) // Olive	#808000	(128,128,0)
		     	 {
				      point.r = 128;
				      point.g = 128;
				      point.b = 0;
			     }
			    else if (segmented_image.at<float>(row, col) == 7) // Teal	#008080	(0,128,128)
			     {
				      point.r = 0;
				      point.g = 128;
				      point.b = 128;
			     }
			    else if (segmented_image.at<float>(row, col) == 8) // Purple	#800080	(128,0,128)
		     	 {
				      point.r = 128;
				      point.g = 0;
				      point.b = 128;
			     }
			    else
		   	     {
				      if ((int)segmented_image.at<float>(row, col) % 2 == 0)
				       {
					        point.r = 255 * segmented_image.at<float>(row, col) / cluster_Index;
					        point.g = 128;
					        point.b = 50;
				       }
				      else
				       {
					        point.r = 0;
					        point.g = 255 * segmented_image.at<float>(row, col) / cluster_Index;
					        point.b = 128;
				       }
            }
         cloud_->push_back(point);
        }
    }
}

void RecursiveSearch(int cluster_Index, int row, int col)
{
  segmented_image.at<float>(row, col) = cluster_Index;

  for(int i=-neighBor; i < neighBor + 1; i++)
   for(int j=-neighBor; j < neighBor + 1; j++)
    {
       if(row + i > 0 & col + j > 0 & row + i < normals_image.rows & col + j < normals_image.cols)
       {
          if(isnan(depth_image.at<float>(row+i, col+j))) continue;
          if(segmented_image.at<float>(row+i, col+j) == 0)
          if(abs(depth_image.at<float>(row+i, col+j) - depth_image.at<float>(row, col)) < 0.01)
          {
             if(abs(normals_image.at<Vec3f>(row, col).val[0] + normals_image.at<Vec3f>(row, col).val[1] + normals_image.at<Vec3f>(row, col).val[2] - 
             normals_image.at<Vec3f>(row+i, col+j).val[0] - normals_image.at<Vec3f>(row+i, col+j).val[1] - normals_image.at<Vec3f>(row+i, col+j).val[2]) < 3)
               RecursiveSearch(cluster_Index, row+i, col+j);
          }
       }
    }
}

void Clustering()
{
  cluster_Index = 0;
  cv::Mat segmented(depth_image.size(), CV_32FC1, Scalar(0));
  segmented_image = segmented.clone();

  for(int row = 0; row < depth_image.rows; row++)
   {
     for(int col = 0; col < depth_image.cols; col++)       
      {
         if(isnan(depth_image.at<float>(row, col)) || row - neighBor < 0 || row + neighBor > depth_image.rows-1 || col - neighBor < 0 || col + neighBor > depth_image.cols-1)
         {
           segmented_image.at<float>(row, col) = -1;
           continue;
         }
         if(segmented_image.at<float>(row, col) == 0)
         {
           cluster_Index++;
           RecursiveSearch(cluster_Index, row, col);
         }  
      }
   }
   //std::cerr << "cluster_Index: " << cluster_Index << "\n";
}

void smoothing()
{
  GaussianBlur( depth_image, depth_image, Size( 5, 5), 0, 0 );
}

void cloud_cb (const sensor_msgs::PointCloud2ConstPtr& cloud_msg)
{
  depthToClould();
  sensor_msgs::PointCloud2 output;
  pcl::PCLPointCloud2 cloud_filtered;
  cloud_->header.frame_id = "camera_depth_optical_frame";
  pcl::toPCLPointCloud2(*cloud_, cloud_filtered);
  pcl_conversions::fromPCL(cloud_filtered, output);
  // Publish the data
  pub.publish (output);
}

void normal_cb (const sensor_msgs::Image::ConstPtr& msg)
{
  cv_bridge::CvImageConstPtr bridge;

  try
  {
    bridge = cv_bridge::toCvCopy(msg, "32FC1");
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("Failed to transform depth image.");
    return;
  }
  depth_image = bridge->image.clone();

  clock_t start, end;
  double time_used;

  start = clock();
  OpenCL_process();
  end = clock();
  if(noData) { std::cerr << "No Data!\n"; return; }
  time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
  if(timePrint) std::cerr << "Total time for data transfering and kernel execution: " << time_used << "\n";

  start = clock();
  Clustering();
  end = clock();
  time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
  if(timePrint) std::cerr << "Time to execute Recursive function on CPU: " << time_used << "\n\n";
  
  cv::imshow("depth", depth_image);
  cv::waitKey(3);
  cv::imshow("Normals", normals_image);
  cv::waitKey(3);
  if(timePrint > 0) timePrint--;
}

int main (int argc, char** argv)
{
  OpenCL_start();
  // Initialize ROS
  ros::init (argc, argv, "my_pcl_tutorial");
  ros::NodeHandle nh;
  ros::NodeHandle n;

  // Create a ROS subscriber for the input point cloud
  ros::Subscriber sub = nh.subscribe ("/camera/depth_registered/points", 1, cloud_cb);
  ros::Subscriber sub_depth = n.subscribe ("/camera/depth/image", 1, normal_cb);

  // Create a ROS publisher for the output point cloud
  pub = nh.advertise<sensor_msgs::PointCloud2> ("output", 1);

  // Spin
  ros::spin ();

      // Finalization 
    ret = clFlush(command_queue);
    ret = clFinish(command_queue);
    ret = clReleaseKernel(kernel);
    ret = clReleaseProgram(program);
    ret = clReleaseMemObject(gpu_depth);
    ret = clReleaseMemObject(gpu_normals);
    ret = clReleaseCommandQueue(command_queue);
    ret = clReleaseContext(context);

    // Free host memory
    free(cpu_depth);
}
