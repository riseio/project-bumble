#include "native_texture_reconstruction.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace bumble::textures {
namespace {
using RGB=std::array<float,3>;
float tap_offset(int tap,unsigned scale,float fraction) {
    return float(tap)*float(scale)*fraction;
}
RGB sample(const Image& im,float x,float y) {
    x=std::clamp(x,0.f,float(im.width-1));y=std::clamp(y,0.f,float(im.height-1));
    const unsigned ix=unsigned(x),iy=unsigned(y),jx=std::min(ix+1,im.width-1),jy=std::min(iy+1,im.height-1);
    const float u=x-float(ix),v=y-float(iy);RGB result{};
    for(unsigned c=0;c<3;++c)
        result[c]=(1-v)*((1-u)*im.rgba[(size_t(iy)*im.width+ix)*4+c]+u*im.rgba[(size_t(iy)*im.width+jx)*4+c])+
            v*((1-u)*im.rgba[(size_t(jy)*im.width+ix)*4+c]+u*im.rgba[(size_t(jy)*im.width+jx)*4+c]);
    return result;
}
RGB low_pass(const Image& im,float x,float y,float radius) {
    RGB total{};
    for(int j=-1;j<=1;++j) for(int i=-1;i<=1;++i) {
        const float weight=float((i?1:2)*(j?1:2))/16.f;
        const auto p=sample(im,x+float(i)*radius,y+float(j)*radius);
        for(unsigned c=0;c<3;++c) total[c]+=weight*p[c];
    }
    return total;
}
Image reconstruct_contours(const Image& source,const Image& current,const std::function<void()>& checkpoint,unsigned scale=9) {
    const auto base=reconstruct(clean_grain(source),scale,checkpoint);
    auto contour=base;
    for(unsigned y=0;y<base.height;++y) for(unsigned x=0;x<base.width;++x) {
        if (x == 0 && checkpoint) checkpoint();
        float xx=0,xy=0,yy=0;
        for(int j=-1;j<=1;++j) for(int i=-1;i<=1;++i) {
            const float sx=float(x)+tap_offset(i,scale,0.5f),sy=float(y)+tap_offset(j,scale,0.5f);
            const auto a=sample(base,sx-scale*0.65f,sy),b=sample(base,sx+scale*0.65f,sy);
            const auto d=sample(base,sx,sy-scale*0.65f),e=sample(base,sx,sy+scale*0.65f);
            const float weight=float((i?1:2)*(j?1:2));
            for(unsigned c=0;c<3;++c) {const float gx=b[c]-a[c],gy=e[c]-d[c];xx+=weight*gx*gx;xy+=weight*gx*gy;yy+=weight*gy*gy;}
        }
        const float energy=xx+yy;
        if(energy<1) continue;
        const float coherence=std::sqrt((xx-yy)*(xx-yy)+4*xy*xy)/energy;
        const double angle=0.5*std::atan2(double(2*xy),double(xx-yy));
        const float tx=float(-std::sin(angle)),ty=float(std::cos(angle));
        const auto centre=sample(base,float(x),float(y));RGB sum{};float weights=0;
        for(int k=-4;k<=4;++k) {
            const float distance=tap_offset(k,scale,0.32f);
            const auto value=sample(base,float(x)+tx*distance,float(y)+ty*distance);
            float difference=0;for(unsigned c=0;c<3;++c) difference+=std::pow(value[c]-centre[c],2);
            const float weight=(5-std::abs(k))/(1+difference/(3*32.f*32.f));
            weights+=weight;for(unsigned c=0;c<3;++c) sum[c]+=weight*value[c];
        }
        const float amount=std::clamp((coherence-0.15f)/0.70f,0.f,1.f);
        for(unsigned c=0;c<3;++c) contour.rgba[(size_t(y)*base.width+x)*4+c]=uint8_t(std::lround(centre[c]+amount*(sum[c]/weights-centre[c])));
    }
    auto out=current;
    const unsigned border=2*scale;
    for(unsigned y=border;y+border<out.height;++y) for(unsigned x=border;x+border<out.width;++x) {
        if (x == border && checkpoint) checkpoint();
        const auto centre=sample(contour,float(x),float(y));RGB narrow{},wide{};
        for(int j=-1;j<=1;++j) for(int i=-1;i<=1;++i) {
            const float weight=float((i?1:2)*(j?1:2))/16;
            const auto a=sample(contour,float(x)+tap_offset(i,scale,0.7f),float(y)+tap_offset(j,scale,0.7f));
            const auto b=sample(contour,float(x)+tap_offset(i,scale,1.4f),float(y)+tap_offset(j,scale,1.4f));
            for(unsigned c=0;c<3;++c){narrow[c]+=weight*a[c];wide[c]+=weight*b[c];}
        }
        RGB lo{255,255,255},hi{};
        const int sx=int(std::lround((x+0.5f)/scale-0.5f)),sy=int(std::lround((y+0.5f)/scale-0.5f));
        for(int j=-2;j<=2;++j) for(int i=-2;i<=2;++i) {
            const auto p=(size_t(std::clamp(sy+j,0,int(source.height)-1))*source.width+unsigned(std::clamp(sx+i,0,int(source.width)-1)))*4;
            for(unsigned c=0;c<3;++c){lo[c]=std::min(lo[c],float(source.rgba[p+c]));hi[c]=std::max(hi[c],float(source.rgba[p+c]));}
        }
        float blend=std::clamp(float(std::min({x,y,out.width-1-x,out.height-1-y})-border)/scale,0.f,1.f);
        blend=blend*blend*(3-2*blend);
        const size_t p=(size_t(y)*out.width+x)*4;
        for(unsigned c=0;c<3;++c) {
            const float residual=1.35f*(centre[c]-narrow[c])+0.50f*(centre[c]-wide[c]);
            const float delta=std::copysign(std::max(0.f,std::abs(residual)-0.8f),residual);
            const float value=std::clamp(centre[c]+std::clamp(delta,-28.f,28.f),lo[c],hi[c]);
            out.rgba[p+c]=uint8_t(std::lround(current.rgba[p+c]+blend*(value-current.rgba[p+c])));
        }
    }
    return out;
}
Image refine_material(const Image& original,const Image& previous,const std::function<void()>& checkpoint) {
    constexpr unsigned scale=9,border=2*scale;
    if(previous.width!=original.width*scale || previous.height!=original.height*scale)
        throw std::runtime_error("Material refinement dimensions differ");
    const auto reference=reconstruct(original,scale,checkpoint);
    auto out=previous;
    for(unsigned y=border;y+border<out.height;++y) for(unsigned x=border;x+border<out.width;++x) {
        if (x == border && checkpoint) checkpoint();
        const auto p=(size_t(y)*out.width+x)*4;
        const auto native=sample(reference,float(x),float(y));
        const auto nativeLow=low_pass(reference,float(x),float(y),0.65f*float(scale));
        const auto prior=sample(previous,float(x),float(y));
        const auto priorLow=low_pass(previous,float(x),float(y),0.65f*float(scale));
        RGB sourceDetail{},priorDetail{},delta{},lo=prior,hi=prior;
        float sourceEnergy=0,priorEnergy=0,correlation=0;
        for(unsigned c=0;c<3;++c) {
            sourceDetail[c]=native[c]-nativeLow[c];priorDetail[c]=prior[c]-priorLow[c];
            sourceEnergy+=sourceDetail[c]*sourceDetail[c];priorEnergy+=priorDetail[c]*priorDetail[c];
            correlation+=sourceDetail[c]*priorDetail[c];
        }
        const float sourceMagnitude=std::sqrt(sourceEnergy);
        const float missing=(sourceMagnitude>1.f && correlation>0.f)
            ?std::clamp(1.f-std::sqrt(priorEnergy)/sourceMagnitude,0.f,1.f):0.f;
        for(unsigned c=0;c<3;++c) {
            const float fine=priorDetail[c];
            delta[c]=0.75f*missing*sourceDetail[c]+3.0f*std::copysign(std::max(0.f,std::abs(fine)-0.35f),fine);
        }
        for(int j=-1;j<=1;++j) for(int i=-1;i<=1;++i) {
            const auto value=sample(previous,float(x)+float(i)*float(scale)*0.85f,float(y)+float(j)*float(scale)*0.85f);
            for(unsigned c=0;c<3;++c){lo[c]=std::min(lo[c],value[c]);hi[c]=std::max(hi[c],value[c]);}
        }
        float amount=1.f;
        for(unsigned c=0;c<3;++c) {
            if(delta[c]>0) amount=std::min(amount,(hi[c]-prior[c])/delta[c]);
            if(delta[c]<0) amount=std::min(amount,(lo[c]-prior[c])/delta[c]);
            if(std::abs(delta[c])>12.f) amount=std::min(amount,12.f/std::abs(delta[c]));
        }
        float seamBlend=std::clamp(float(std::min({x,y,out.width-1-x,out.height-1-y})-border)/float(scale),0.f,1.f);
        seamBlend=seamBlend*seamBlend*(3.f-2.f*seamBlend);
        for(unsigned c=0;c<3;++c) out.rgba[p+c]=uint8_t(std::lround(prior[c]+seamBlend*amount*delta[c]));
    }
    return out;
}

}

Image reconstruct_material(const Image& source,const std::function<void()>& checkpoint) {
    const auto boundary=filter_surface(reconstruct(restore_detail(clean_grain(source)),9,checkpoint),9,checkpoint);
    return refine_material(source,reconstruct_contours(source,boundary,checkpoint),checkpoint);
}
}
