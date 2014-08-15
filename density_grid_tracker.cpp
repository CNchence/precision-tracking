/*
 * density_grid_tracker.cpp
 *
 *  Created on: Sep 1, 2013
 *      Author: davheld
 */

#include "density_grid_tracker.h"

#include <stdlib.h>
#include <numeric>
#include <boost/math/constants/constants.hpp>

#include <pcl/common/common.h>

#include "model_builder.h"

namespace {

// We assume that there are this many independent points per object.  Beyond
// this many, we discount the measurement model accordingly.
const double kMaxDiscountPoints = 150.0;

// How far to spill over in the density grid (number of sigmas).
const double kSpilloverRadius = 2.0;

// Factor to multiply the sensor resolution for our measurement model.
// We model each point as a Gaussian: exp(-x^2 / 2 sigma^2)
// With sigma^2 = (sensor_resolution * kSigmaFactor)^2 + other terms.
const double kSigmaFactor = 0.5;

// Factor to multiply the particle sampling resolution for our measurement  model.
// We model each point as a Gaussian: exp(-x^2 / 2 sigma^2)
// With sigma^2 = (sampling_resolution * kSigmaGridFactor)^2 + other terms.
const double kSigmaGridFactor = 1;

// The noise in our sensor which is independent of the distance to the tracked object.
// We model each point as a Gaussian: exp(-x^2 / 2 sigma^2)
// With sigma^2 = kMinMeasurementVariance^2 + other terms.
const double kMinMeasurementVariance = 0.03;

// We add this to our Gaussian so we don't give 0 probability to points
// that don't align.
// We model each point as a Gaussian: exp(-x^2 / 2 sigma^2) + kSmoothingFactor
const double kSmoothingFactor = 0.8;

// We multiply our log measurement probability by this factor, to decrease
// our confidence in the measurement model (e.g. to take into account
// dependencies between neighboring points).
const double kMeasurementDiscountFactor = 1;

const bool k_NNTracking = false;

const double pi = boost::math::constants::pi<double>();

// Total size = 3.7 GB
// At a resolution of 1.2 cm, a 10 m wide object will take 1000 cells.
const int kMaxXSize = k_NNTracking ? 1 : 1000;
const int kMaxYSize = k_NNTracking ? 1 : 1000;
// At a resolution of 1.2 cm, a 5 m tall object will take 500 cells.
const int kMaxZSize = k_NNTracking ? 1 : 500;

using std::vector;
using std::pair;
using std::max;
using std::min;

}  // namespace

// Initialize the density grid to all have log(kSmoothingFactor), so we do
// not give a probability of 0 to any location.
DensityGridTracker::DensityGridTracker()
  : fast_functions_(FastFunctions::getInstance()),
    density_grid_(kMaxXSize, vector<vector<double> >(
        kMaxYSize, vector<double>(kMaxZSize, log(kSmoothingFactor)))) {
}

DensityGridTracker::~DensityGridTracker() {
	// TODO Auto-generated destructor stub
}

void DensityGridTracker::track(
    const double xy_stepSize,
    const double z_stepSize,
    const pair <double, double>& xRange,
    const pair <double, double>& yRange,
    const pair <double, double>& zRange,
    const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr& current_points,
    const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr& prev_points,
    const Eigen::Vector3f& current_points_centroid,
		const MotionModel& motion_model,
		const double horizontal_distance,
		const double down_sample_factor,
    ScoredTransforms<ScoredTransformXYZ>* transforms) {
	// Find all candidate xyz transforms.
  vector<XYZTransform> xyz_transforms;
  createCandidateXYZTransforms(xy_stepSize, z_stepSize,
      xRange, yRange, zRange, &xyz_transforms);

  // Get scores for each of the xyz transforms.
  scoreXYZTransforms(
  		current_points, prev_points, current_points_centroid,
  		xy_stepSize, z_stepSize,
  		xyz_transforms, motion_model, horizontal_distance, down_sample_factor,
      transforms);
}

void DensityGridTracker::createCandidateXYZTransforms(
    const double xy_step_size,
    const double z_step_size,
    const std::pair <double, double>& xRange,
    const std::pair <double, double>& yRange,
    const std::pair <double, double>& zRange_orig,
    std::vector<XYZTransform>* transforms) {
  std::pair<double, double> zRange;
  if (z_step_size > fabs(zRange_orig.first)) {
    zRange.first = 0;
    zRange.second = 0;
  } else {
    zRange.first = zRange_orig.first;
    zRange.second = zRange_orig.second;
  }

  //printf("xRange: %lf to %lf, stepSize: %lf\n",  xRange.first,  xRange.second, xy_step_size);
  //printf("yRange: %lf to %lf, stepSize: %lf\n",  yRange.first,  yRange.second, xy_step_size);
  //printf("zRange: %lf to %lf, stepSize: %lf\n",  zRange.first,  zRange.second, z_step_size);

  // Compute the number of transforms along each direction.
  const int num_x_locations = (xRange.second - xRange.first) / xy_step_size;
  const int num_y_locations = (yRange.second - yRange.first) / xy_step_size;
  int num_z_locations;
  if (z_step_size == 0) {
    num_z_locations = 1;
  } else {
    num_z_locations = (zRange.second - zRange.first) / z_step_size;
  }

  // Reserve space for all of the transforms.
  transforms->reserve(num_x_locations * num_y_locations * num_z_locations);

  const double volume = pow(xy_step_size, 2) * z_step_size;

  // Create a list of candidate transforms.
  for (double x = xRange.first; x <= xRange.second; x += xy_step_size){
    for (double y = yRange.first; y <= yRange.second; y += xy_step_size){
      XYZTransform transform(x, y, 0, volume);
      transforms->push_back(transform);
    }
  }


  /*if (xy_step_size == 0) {
    printf("Error - xy step size must be > 0");
    exit(1);
  }

  if (z_step_size == 0) {
    printf("Error - z step size must be > 0");
    exit(1);
  }

  // Make sure we hit 0 in our z range, in case the step is too large.
  std::pair<double, double> zRange;
  if (z_step_size > fabs(zRange.second - zRange_orig.first)) {
    zRange.first = 0;
    zRange.second = 0;
  } else {
    zRange.first = zRange_orig.first;
    zRange.second = zRange_orig.second;
  }

  // Compute the number of transforms along each direction.
  const int num_x_locations = (xRange.second - xRange.first) / xy_step_size + 1;
  const int num_y_locations = (yRange.second - yRange.first) / xy_step_size + 1;
  const int num_z_locations = (zRange.second - zRange.first) / z_step_size + 1;

  // Reserve space for all of the transforms.
  transforms->reserve(num_x_locations * num_y_locations * num_z_locations);

  const double volume = pow(xy_step_size, 2) * z_step_size;

  // Create a list of candidate transforms.
  for (double x = xRange.first; x <= xRange.second; x += xy_step_size){
    for (double y = yRange.first; y <= yRange.second; y += xy_step_size){
      for (double z = zRange.first; z <= zRange.second; z +=z_step_size){
        XYZTransform transform(x, y, z, volume);
        transforms->push_back(transform);
      }
    }
  }*/
}

void DensityGridTracker::scoreXYZTransforms(
    const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr& current_points,
    const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr& prev_points,
    const Eigen::Vector3f& current_points_centroid,
    const double xy_stepSize,
    const double z_stepSize,
    const vector<XYZTransform>& transforms,
    const MotionModel& motion_model,
    const double horizontal_distance,
    const double down_sample_factor,
    ScoredTransforms<ScoredTransformXYZ>* scored_transforms) {
  // Determine the size and minimum density for the density grid.
  computeDensityGridSize(prev_points, xy_stepSize, z_stepSize,
      horizontal_distance, down_sample_factor);

  computeDensityGrid(prev_points);

  const size_t num_transforms = transforms.size();

  // Compute scores for all of the transforms using the density grid.
  scored_transforms->clear();
  scored_transforms->reserve(num_transforms);
  for(size_t i = 0; i < num_transforms; ++i){
    const XYZTransform& transform = transforms[i];
    const double x = transform.x;
    const double y = transform.y;
    const double z = transform.z;
    const double volume = transform.volume;

    const double log_prob = getLogProbability(current_points, min_pt_,
        xy_grid_step_, z_grid_step_, motion_model, x, y, z);

    // Save the complete transform with its log probability.
    const ScoredTransformXYZ scored_transform(x, y, z, log_prob, volume);
    scored_transforms->addScoredTransform(scored_transform);
  }
}

void DensityGridTracker::computeDensityGridSize(
    const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr& prev_points,
    const double xy_stepSize,
    const double z_stepSize,
    const double horizontal_distance,
    const double down_sample_factor) {
  // Get the appropriate size for the grid.
  xy_grid_step_ = xy_stepSize;
  z_grid_step_ = z_stepSize;

  // Downweight all points beyond kMaxDiscountPoints because they are not
  // all independent.
  if (prev_points->size() < kMaxDiscountPoints) {
      discount_factor_ = kMeasurementDiscountFactor;
  } else {
      discount_factor_ = kMeasurementDiscountFactor *
          (kMaxDiscountPoints / prev_points->size());
  }

  // Find the min and max of the previous points.
  pcl::PointXYZRGB max_pt;
  pcl::getMinMax3D(*prev_points, min_pt_, max_pt);

  const double epsilon = 0.0001;

  // We add one grid step of padding to allow for inexact matches.  The outer
  // grid cells are kept empty and are used to represent the empty space
  // around the tracked object.
  min_pt_.x -= (2 * xy_grid_step_ + epsilon);
  min_pt_.y -= (2 * xy_grid_step_ + epsilon);

  // If we have a large step size in the z-direction, we want to center
  // the object within the grid cell.
  const double z_range = max_pt.z - min_pt_.z;
  const double z_centering = fabs(z_grid_step_ - z_range) / 2;
  min_pt_.z -= (2 * z_grid_step_ + z_centering);

  // We add one grid step of padding to allow for inexact matches.  The outer
  // grid cells are kept empty and are used to represent the empty space
  // around the tracked object.
  max_pt.x += 2 * xy_grid_step_;
  max_pt.y += 2 * xy_grid_step_;
  max_pt.z += 2 * z_grid_step_;

  // Find the appropriate size for the density grid.
  xSize_ = min(kMaxXSize, max(1, static_cast<int>(
      ceil((max_pt.x - min_pt_.x) / xy_grid_step_))));
  ySize_ = min(kMaxYSize, max(1, static_cast<int>(
      ceil((max_pt.y - min_pt_.y) / xy_grid_step_))));
  zSize_ = min(kMaxZSize, max(1, static_cast<int>(
      ceil((max_pt.z - min_pt_.z) / z_grid_step_))));

  // Reset the density grid to the default value.
  const double default_val = log(kSmoothingFactor);
  for (int i = 0; i < xSize_; ++i) {
    for (int j = 0; j < ySize_; ++j) {
      std::fill(
          density_grid_[i][j].begin(),
          density_grid_[i][j].begin() + zSize_, default_val);
    }
  }

  // TODO - pass this as an input parameter.
  // Compute the sensor horizontal resolution
  const double velodyne_horizontal_res_actual = 2 * horizontal_distance * tan(.18 / 2.0 * pi / 180.0);

  // The effective resolution = resolution / downsample factor.
  const double velodyne_horizontal_res = velodyne_horizontal_res_actual / down_sample_factor;

  // The vertical resolution for the Velodyne is 2.2 * the horizontal resolution.
  const double velodyne_vertical_res = 2.2 * velodyne_horizontal_res;

  // Compute the different sources of error in the xy directions.
  const double sampling_error_xy = kSigmaGridFactor * xy_stepSize;
  const double resolution_error_xy = velodyne_horizontal_res * kSigmaFactor;
  const double noise_error_xy = kMinMeasurementVariance;

  // The variance is a combination of these 3 sources of error.
  spillover_sigma_xy_ = sqrt(pow(sampling_error_xy, 2) +
                             pow(resolution_error_xy, 2) +
                             pow(noise_error_xy, 2));

  // Compute the different sources of error in the z direction.
  const double sampling_error_z = 0 ; //kSigmaGridFactor * z_stepSize;
  const double resolution_error_z = velodyne_vertical_res * kSigmaFactor;
  const double noise_error_z = kMinMeasurementVariance;

  // The variance is a combination of these 3 sources of error.
  spillover_sigma_z_ = sqrt(pow(sampling_error_z, 2) + pow(resolution_error_z, 2) +
                            pow(noise_error_z, 2));

  // In our discrete grid, we want to compute the Gaussian for a certian
  // number of grid cells away from the point.
  num_spillover_steps_xy_ =
      ceil(kSpilloverRadius * spillover_sigma_xy_ / xy_grid_step_ - 1);
  // Our implementation requires that we spill over at least 1 cell in the
  // z direction.
  num_spillover_steps_z_ =
      max(1.0, ceil(kSpilloverRadius * spillover_sigma_z_ / z_grid_step_ - 1));

  min_density_ = kSmoothingFactor;
}

void DensityGridTracker::computeDensityGrid(
    const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr& points) {
  // Apply this offset when converting from the point location to the index.
  const double x_offset = -min_pt_.x / xy_grid_step_;
  const double y_offset = -min_pt_.y / xy_grid_step_;
  const double z_offset = -min_pt_.z / z_grid_step_;

  // Convert sigma to a factor such that
  // exp(-x^2 * grid_size^2 / 2 sigma^2) = exp(x^2 * factor)
  // where x is the number of grid steps.
  const double xy_exp_factor =
      -1.0 * pow(xy_grid_step_, 2) / (2 * pow(spillover_sigma_xy_, 2));
  const double z_exp_factor =
      -1.0 * pow(z_grid_step_, 2) / (2 * pow(spillover_sigma_z_, 2));

  // For any given point, the density falls off as a Gaussian to
  // neighboring regions.
  // Pre-compute the density spillover for different cell distances.
  vector<vector<vector<double> > > spillovers(
        num_spillover_steps_xy_ + 1, vector<vector<double> >(
          num_spillover_steps_xy_ + 1, vector<double>(
            num_spillover_steps_z_ + 1)));
  for (int i = 0; i <= num_spillover_steps_xy_; ++i) {
    const int i_dist_sq = pow(i, 2);

    for (int j = 0; j <= num_spillover_steps_xy_; ++j) {
      const int j_dist_sq = pow(j, 2);
      const double log_xy_density = (i_dist_sq + j_dist_sq) * xy_exp_factor;

      for (int k = 0; k <= num_spillover_steps_z_; ++k) {
        const int k_dist_sq = pow(k, 2);
        const double log_z_density = k_dist_sq * z_exp_factor;

        spillovers[i][j][k] = log(
              exp(log_xy_density + log_z_density) + min_density_);
      }
    }
  }

  if (num_spillover_steps_z_ == 0) {
    printf("Error - we assume that we are spilling at least 1 in the"
           "z-direction\n");
  }

  // Build the density grid
  size_t num_points = points->size();

  for (size_t i = 0; i < num_points; ++i) {
    const pcl::PointXYZRGB& pt = (*points)[i];

    // Find the indices for this point.
    const int x_index = round(pt.x / xy_grid_step_ + x_offset);
    const int y_index = round(pt.y / xy_grid_step_ + y_offset);
    const int z_index = round(pt.z / z_grid_step_ + z_offset);

    // Spill the probability density into neighboring regions as a Guassian
    // (but not to the borders, which represent the empty space around the
    // tracked object)
    const int max_x_index =
        max(1, min(xSize_ - 2, x_index + num_spillover_steps_xy_));
    const int max_y_index =
        max(1, min(ySize_ - 2, y_index + num_spillover_steps_xy_));

    const int min_x_index =
        min(xSize_ - 2, max(1, x_index - num_spillover_steps_xy_));
    const int min_y_index =
        min(ySize_ - 2, max(1, y_index - num_spillover_steps_xy_));

    if (num_spillover_steps_z_ > 1) {
      const int max_z_index =
          max(1, min(zSize_ - 2, z_index + num_spillover_steps_z_));
      const int min_z_index =
          max(1, min(zSize_ - 2, z_index - num_spillover_steps_z_));

      // Spill the probability into neighboring cells as a Guassian.
      for (int x_spill = min_x_index; x_spill <= max_x_index; ++x_spill){
        const int x_diff = abs(x_index - x_spill);

        for (int y_spill = min_y_index; y_spill <= max_y_index; ++y_spill) {
          const int y_diff = abs(y_index - y_spill);

          for (int z_spill = min_z_index; z_spill <= max_z_index; ++z_spill) {
            const int z_diff = abs(z_index - z_spill);

          const double spillover = spillovers[x_diff][y_diff][z_diff];

          density_grid_[x_spill][y_spill][z_spill] =
              max(density_grid_[x_spill][y_spill][z_spill], spillover);
          }
        }
      }
    } else {
      // This is an optimization that we can do if we are only spilling
      // over 1 grid cell, which happens fairy often.

      // For z, we only spill up one and down one, so pre-compute these.
      const int z_spill = std::min(std::max(1, z_index), zSize_ - 2);
      const int z_spill_up = std::min(z_spill + 1, zSize_ - 2);
      const int z_spill_down = std::max(1, z_spill - 1);

      // Spill the probability into neighboring cells as a Guassian.
      for (int x_spill = min_x_index; x_spill <= max_x_index; ++x_spill){
        const int x_diff = abs(x_index - x_spill);

        for (int y_spill = min_y_index; y_spill <= max_y_index; ++y_spill) {
          const int y_diff = abs(y_index - y_spill);

          const double spillover0 = spillovers[x_diff][y_diff][0];

          density_grid_[x_spill][y_spill][z_spill] =
              max(density_grid_[x_spill][y_spill][z_spill], spillover0);

          const double spillover1 = spillovers[x_diff][y_diff][1];

          density_grid_[x_spill][y_spill][z_spill_up] =
              max(density_grid_[x_spill][y_spill][z_spill_up], spillover1);

          density_grid_[x_spill][y_spill][z_spill_down] =
              max(density_grid_[x_spill][y_spill][z_spill_down], spillover1);

        }
      }
    }
  }
}

double DensityGridTracker::getLogProbability(
    const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr& current_points,
		const pcl::PointXYZRGB& minPt,
    const double xy_gridStep,
    const double z_gridStep,
		const MotionModel& motion_model,
		const double x,
		const double y,
    const double z) const {
  // Amount of total log probability density for the given alignment.
  double total_log_density = 0;

  // Offset to apply to each point to get the new position.
	const double x_offset = (x - minPt.x) / xy_gridStep;
	const double y_offset = (y - minPt.y) / xy_gridStep;
  const double z_offset = (z - minPt.z) / z_gridStep;

  // Iterate over every point and look up its log probability density
  // in the density grid.
	const size_t num_points = current_points->size();
  for (size_t i = 0; i < num_points; ++i) {
    // Extract the point so we can compute its probability.
  	const pcl::PointXYZRGB& pt = (*current_points)[i];

    // We shift each point based on the proposed alignment, to try to
    // align the current points with the previous points.  We then
    // divide by the grid step to find the appropriate cell in the density
    // grid.
    const int x_index_shifted =
        min(max(0, static_cast<int>(round(pt.x / xy_gridStep + x_offset))),
            xSize_ - 1);
    const int y_index_shifted =
        min(max(0, static_cast<int>(round(pt.y / xy_gridStep + y_offset))),
            ySize_ - 1);
    const int z_index_shifted =
        min(max(0, static_cast<int>(round(pt.z / z_gridStep + z_offset))),
            zSize_ - 1);

    // Look up the log density of this grid cell and add to the total density.
    total_log_density +=
        density_grid_[x_index_shifted][y_index_shifted][z_index_shifted];
  }

  // Compute the motion model probability.
  const double motion_model_prob = motion_model.computeScore(x, y, z);

  // Compute the log measurement probability.
  const double log_measurement_prob = total_log_density;

  // Combine the motion model score with the (discounted) measurement score to
  // get the final log probability.
  const double log_prob =
      log(motion_model_prob) + discount_factor_ * log_measurement_prob;

  return log_prob;
}
