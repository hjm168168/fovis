// ISSUES:
// - StereoCalibration from stereo_depth.cpp should probably be included in its own cpp
// - disparity values with no depth return 0. Should this be set to NAN in advance? in the driver?
// - Is my interpolation actually correct? I haven't tested the math/values
// - Remove unnecessary and unused parameters in header file.

#include "stereo_disparity.hpp"

#include <cstdio>
#include <iostream>

#include <emmintrin.h>

#include "visual_odometry.hpp"
#include "tictoc.hpp"
#include "fast.hpp"
#include "sad.hpp"
#include "rectification.hpp"
#include "feature_match.hpp"
#include "refine_feature_match.hpp"
#include "refine_motion_estimate.hpp"
#include "internal_utils.hpp"
#include "stereo_rectify.hpp"

#define MIN_DISPARITY 0

namespace fovis
{

StereoDisparity::StereoDisparity(const StereoCalibration* calib,
                         const VisualOdometryOptions& options) :
    _calib(calib),
    _width(calib->getWidth()),
    _height(calib->getHeight()),
    _fast_threshold_min(5),
    _fast_threshold_max(70),
    _options(options)
{
  const VisualOdometryOptions& defaults(VisualOdometry::getDefaultOptions());

  _feature_window_size = optionsGetIntOrFromDefault(_options, "feature-window-size", defaults);
  _num_pyramid_levels = optionsGetIntOrFromDefault(_options, "max-pyramid-level", defaults);
  _fast_threshold = optionsGetIntOrFromDefault(_options, "fast-threshold", defaults);
  _use_adaptive_threshold = optionsGetBoolOrFromDefault(_options, "use-adaptive-threshold", defaults);
  _max_refinement_displacement = optionsGetDoubleOrFromDefault(_options, "stereo-max-refinement-displacement", defaults);
  _require_mutual_match = optionsGetBoolOrFromDefault(_options, "stereo-require-mutual-match", defaults);
  _max_disparity = optionsGetIntOrFromDefault(_options, "stereo-max-disparity", defaults);
  _max_dist_epipolar_line = optionsGetDoubleOrFromDefault(_options, "stereo-max-dist-epipolar-line", defaults);
  _target_pixels_per_feature = optionsGetIntOrFromDefault(_options, "target-pixels-per-feature", defaults);
  _fast_threshold_adaptive_gain = optionsGetDoubleOrFromDefault(_options, "fast-threshold-adaptive-gain", defaults);

  _matched_right_keypoints_per_level.resize(_num_pyramid_levels);

  _disparity_data = new float[ _width* _height];

  _uvd1_to_xyz = new Eigen::Matrix4d(calib->getUvdToXyz());

  _matches_capacity = 200;
  _matches = new FeatureMatch[_matches_capacity];
  _num_matches = 0;
}

StereoDisparity::~StereoDisparity()
{
  delete[] _matches;
}

void
StereoDisparity::setDisparityData(const float* disparity_data)
{
  // copy disparity image
  int num_disparity_pixels = _width * _height;
  memcpy(_disparity_data, disparity_data, num_disparity_pixels * sizeof(float));
}

bool
StereoDisparity::haveXyz(int u, int v)
{
  float disp = _disparity_data[ (int) v*_width + (int) u] ;

  // disp ==0 mean no disparity for that return
  // TODO: should this be done more in advance?
  if (disp == 0){
    return false;
  }
  return true;
}

Eigen::Vector3d
StereoDisparity::getXyzValues(int u, int v, float disparity)
{
  Eigen::Vector4d uvd1( (double) u, (double) v, disparity, 1);
  Eigen::Vector4d xyzw = (*_uvd1_to_xyz) * uvd1;
  return (xyzw.head<3>() / xyzw.w());
}


void
StereoDisparity::getXyz(OdometryFrame * odom_frame)
{
  int num_levels = odom_frame->getNumLevels();
  for(int level_num=0; level_num < num_levels; ++level_num) {
    PyramidLevel* level = odom_frame->getLevel(level_num);
    int num_kp = level->getNumKeypoints();
    //std::cout << level_num << " | number of keypoints: " << num_kp << "\n";

    for(int kp_ind=0; kp_ind < num_kp; ++kp_ind) {
      KeypointData* kpdata(level->getKeypointData(kp_ind));

      int u = (int)(kpdata->rect_base_uv(0)+0.5);
      int v = (int)(kpdata->rect_base_uv(1)+0.5);

      kpdata->disparity = _disparity_data[ (int) v*_width + (int) u];
      if (kpdata->disparity == 0){ // disp ==0 if no disparity available given
        kpdata->disparity = NAN;
        kpdata->has_depth = false;
        kpdata->xyzw = Eigen::Vector4d(NAN, NAN, NAN, NAN);
        kpdata->xyz = Eigen::Vector3d(NAN, NAN, NAN);
      } else {
        kpdata->has_depth = true;
        kpdata->xyz = getXyzValues( u, v, kpdata->disparity );
        kpdata->xyzw.head<3>() = kpdata->xyz;
        kpdata->xyzw.w() = 1;
      }
    }
  }
}

void
StereoDisparity::refineXyz(FeatureMatch * matches,
                       int num_matches,
                       OdometryFrame * odom_frame)
{
  for (int m_ind = 0; m_ind < num_matches; m_ind++) {
    FeatureMatch& match = matches[m_ind];
    if (match.status == MATCH_NEEDS_DEPTH_REFINEMENT) {
      if (getXyzInterp(&match.refined_target_keypoint)) {
        match.status = MATCH_OK;
      } else {
        match.status = MATCH_REFINEMENT_FAILED;
        match.inlier = false;
      }
    }
  }
}

// This mirrors the interpolation in depth_image.cpp
bool
StereoDisparity::getXyzInterp(KeypointData* kpdata)
{
  // 1. find the fractional pixel left, right and right&down from nearest unit pixel:
  // mfallon: this seems to support non-640x480 kinect data but I'm not sure its necessary for disparity data
  double _x_scale =1;
  double _y_scale =1;
  float u_f = kpdata->rect_base_uv(0);
  float v_f = kpdata->rect_base_uv(1);
  float v_f_d = v_f * _y_scale;
  float u_f_d = u_f * _x_scale;
  int v = (int)v_f_d;
  int u = (int)u_f_d;
  float wright  = (u_f_d - u);
  float wbottom = (v_f_d - v);

  // can't handle borders
  assert(u >= 0 && v >= 0 && u < _width - 1 && v < _height - 1);

  float w[4] = {
    (1 - wright) * (1 - wbottom),
    wright * (1 - wbottom),
    (1 - wright) * wbottom,
    wright * wbottom
  }; // weights sum to unity

  // 2. find corresponding disparities, u and v values around the non-unit pixel
  int index = v * _width + u;
  double disparities[4] = {
    _disparity_data[index ],
    _disparity_data[index + 1],
    _disparity_data[index + _width ],
    _disparity_data[index + _width  + 1]
  };
  for(int i = 0; i<4; i++){
    if( (disparities[i]==0) ){ // set to a known value - TODO: find a better way to do this or assume this in advance
      disparities[i] = NAN;
    }
  }
  int u_vals[4] = {u, u+1,   u, u+1};
  int v_vals[4] = {v,   v, v+1, v+1};

  // missing any depth data for surrounding pixels?
  int num_missing_data = 0;
  for(int i = 0; i<4; i++)
    if(isnan(disparities[i]))
      num_missing_data++;

  if(num_missing_data == 4) { // missing all surrounding depth data.
    return false;
  }

  // 3. Interpolate the edge points to give refined x,y,z for pixel
  if(num_missing_data) {
    // if any of the surrounding depth data is missing, just clamp to the
    // nearest pixel.  interpolation gets messy if we try to do otherwise
    float wmax = -1;
    for(int i=0; i<4; i++) {
      if(isnan(disparities[i]) && w[i] > wmax) {
        kpdata->xyz = getXyzValues( u_vals[i]  , v_vals[i] , disparities[i] );
        wmax = w[i];
      }
    }
  } else {
    Eigen::Vector3d xyz_combined = Eigen::Vector3d (0.,0.,0.);
    // TODO: this simply averages the xyz pixel locations.
    // Should this be done in a manner e.g. averaging the disparity or with reprojective covariance?
    for(int i=0; i<4; i++){
      Eigen::Vector3d xyz = getXyzValues( u_vals[i]  , v_vals[i] , disparities[i] );
      xyz_combined= xyz_combined + xyz * w[i];
    }
    kpdata->xyz = xyz_combined;
  }
  kpdata->xyzw.head<3>() = kpdata->xyz;
  kpdata->xyzw.w() = 1;
  return true;
}

} /*  */
