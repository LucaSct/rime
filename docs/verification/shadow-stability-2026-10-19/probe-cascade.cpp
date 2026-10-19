// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <cmath>
#include <cstdio>
#include <algorithm>
#include "rime/render/lighting/shadows.hpp"
using namespace rime;
int main(){
 render::LightingSettings s;s.cascade_count=3;s.shadow_map_resolution=2048;
 render::CascadeInputs in;in.fov_y=0.87266f;in.aspect=16.f/9;in.z_near=.1f;in.z_far=300;in.light_dir=core::normalize(core::Vec3{.4f,-1.f,.3f});
 render::CascadeFit previous;double worst=0,scale_worst=0;unsigned bad=0;
 for(int i=0;i<600;i++){
  float x=76.f+i*.007f, y=1.5f, z=2.f+i*.003f;float yaw=i*.003f,pitch=.03f*std::sin(i*.07f);
  core::Vec3 eye{x,y,z},direction{std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch)};
  in.camera_view=core::look_at(eye,eye+direction,{0,1,0});auto f=render::compute_cascades(in,s);
  if(i)for(unsigned c=0;c<f.count;c++){
   for(int row=0;row<2;row++){
    double length=0,prevlength=0;
    for(int j=0;j<3;j++){length+=double(f.view_proj[c].at(row,j))*f.view_proj[c].at(row,j);prevlength+=double(previous.view_proj[c].at(row,j))*previous.view_proj[c].at(row,j);}
    length=std::sqrt(length);prevlength=std::sqrt(prevlength);scale_worst=std::max(scale_worst,std::abs(length/prevlength-1));
    // A fixed world origin's shadow UV phase can change only by whole texels.
    double delta=0; const double point[4]={76,0,-30,1}; for(int j=0;j<4;j++) delta+=(double(f.view_proj[c].at(row,j))-previous.view_proj[c].at(row,j))*point[j]; delta*=s.shadow_map_resolution*.5;
    double drift=std::abs(delta-std::round(delta));worst=std::max(worst,drift);if(drift>.001)bad++;
   }
  }
  previous=f;
 }
 std::printf("camera frames=600, max fractional-texel grid-phase drift=%.9f, max relative XY scale variation=%.9f, phase failures over .001 texel=%u\n",worst,scale_worst,bad);
 return worst>.001?1:0;
}
