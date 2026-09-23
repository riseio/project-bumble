#include "native_texture_reconstruction.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace bumble::textures {
namespace {
using Bytes = std::vector<uint8_t>;
void put(Bytes& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset+i) = uint8_t(value >> (8*i));
}
void validate(const Image& image) {
    if (!image.width || !image.height || image.width > 4096 || image.height > 4096 ||
        image.rgba.size() != size_t(image.width) * image.height * 4)
        throw std::runtime_error("Invalid reconstruction image dimensions/payload");
}
float slope(float a, float b) {
    return a * b <= 0 ? 0 : 2 * a * b / (a + b);
}
float interpolate(float a, float b, float c, float d, float t) {
    const float t2 = t*t, t3 = t2*t;
    const float result = (2*t3-3*t2+1)*b + (t3-2*t2+t)*slope(b-a,c-b) +
        (-2*t3+3*t2)*c + (t3-t2)*slope(c-b,d-c);
    return std::clamp(result, std::min(b,c), std::max(b,c));
}
Image mip(const Image& src) {
    Image out{std::max(1U,src.width/2),std::max(1U,src.height/2),{}};
    out.rgba.resize(size_t(out.width)*out.height*4);
    for (uint32_t y=0;y<out.height;++y) for (uint32_t x=0;x<out.width;++x) {
        const uint32_t x0=x*src.width, x1=(x+1)*src.width;
        const uint32_t y0=y*src.height, y1=(y+1)*src.height;
        std::array<uint64_t,4> sum{};
        for(uint32_t sy=y0/out.height;sy<(y1+out.height-1)/out.height;++sy)
            for(uint32_t sx=x0/out.width;sx<(x1+out.width-1)/out.width;++sx) {
                const uint64_t weight=(std::min(x1,(sx+1)*out.width)-std::max(x0,sx*out.width))*
                    uint64_t(std::min(y1,(sy+1)*out.height)-std::max(y0,sy*out.height));
                for(unsigned c=0;c<4;++c) sum[c]+=src.rgba[(size_t(sy)*src.width+sx)*4+c]*weight;
            }
        const uint64_t total=uint64_t(src.width)*src.height;
        for(unsigned c=0;c<4;++c) out.rgba[(size_t(y)*out.width+x)*4+c]=uint8_t((sum[c]+total/2)/total);
    }
    return out;
}
}

Image clean_grain(const Image& src) {
    validate(src);
    Image out=src;
    const auto luma=[&](uint32_t x,uint32_t y) {
        const auto i=(size_t(y)*src.width+x)*4;
        return (54*int(src.rgba[i])+183*int(src.rgba[i+1])+19*int(src.rgba[i+2])+128)/256;
    };
    for(uint32_t y=2;y+2<src.height;++y) for(uint32_t x=2;x+2<src.width;++x) {
        const int centre=luma(x,y);
        int residual=0,total=0;
        for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
            const int difference=luma(uint32_t(int(x)+dx),uint32_t(int(y)+dy))-centre;
            const int weight=std::max(0,8-std::abs(difference))*(dx==0?2:1)*(dy==0?2:1);
            residual+=weight*difference; total+=weight;
        }
        int delta=(std::abs(residual)+total/2)/total*(residual<0?-1:1);
        const auto distance=std::min({x,y,src.width-1-x,src.height-1-y});
        const int limit=distance==2?1:2;
        int minimum=-limit,maximum=limit;
        const auto i=(size_t(y)*src.width+x)*4;
        for(unsigned c=0;c<3;++c) {
            minimum=std::max(minimum,-int(src.rgba[i+c]));
            maximum=std::min(maximum,255-int(src.rgba[i+c]));
        }
        delta=std::clamp(delta,minimum,maximum);
        for(unsigned c=0;c<3;++c) out.rgba[i+c]=uint8_t(int(src.rgba[i+c])+delta);
    }
    return out;
}

Image restore_detail(const Image& src) {
    validate(src);
    Image out=src;
    const auto luma=[&](uint32_t x,uint32_t y) {
        const auto i=(size_t(y)*src.width+x)*4;
        return (54*int(src.rgba[i])+183*int(src.rgba[i+1])+19*int(src.rgba[i+2])+128)/256;
    };
    for(uint32_t y=2;y+2<src.height;++y) for(uint32_t x=2;x+2<src.width;++x) {
        int sum=0;
        std::array<int,3> low{255,255,255},high{};
        for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
            const auto sx=uint32_t(int(x)+dx),sy=uint32_t(int(y)+dy);
            sum+=luma(sx,sy)*(dx==0?2:1)*(dy==0?2:1);
            for(unsigned c=0;c<3;++c) {
                const int value=src.rgba[(size_t(sy)*src.width+sx)*4+c];
                low[c]=std::min(low[c],value); high[c]=std::max(high[c],value);
            }
        }
        const int residual=16*luma(x,y)-sum;
        const int magnitude=std::max(0,std::abs(residual)-16);
        int delta=(magnitude*4+8)/16*(residual<0?-1:1);
        const auto distance=std::min({x,y,src.width-1-x,src.height-1-y});
        if(distance==2) delta/=2;
        int minimum=-24,maximum=24;
        const auto i=(size_t(y)*src.width+x)*4;
        for(unsigned c=0;c<3;++c) {
            minimum=std::max(minimum,low[c]-int(src.rgba[i+c]));
            maximum=std::min(maximum,high[c]-int(src.rgba[i+c]));
        }
        delta=std::clamp(delta,minimum,maximum);
        for(unsigned c=0;c<3;++c) out.rgba[i+c]=uint8_t(int(src.rgba[i+c])+delta);
    }
    return out;
}

Image reconstruct(const Image& src, uint32_t scale, const std::function<void()>& checkpoint) {
    validate(src);
    if(!scale || scale>16 || src.width>4096/scale || src.height>4096/scale)
        throw std::runtime_error("Invalid reconstruction scale");
    Image out{src.width*scale,src.height*scale,{}};
    out.rgba.resize(size_t(out.width)*out.height*4);
    std::vector<float> horizontal(size_t(src.height)*out.width*4);
    for(uint32_t y=0;y<src.height;++y) for(uint32_t x=0;x<out.width;++x) {
        if (x == 0 && checkpoint) checkpoint();
        const float u=(float(x)+0.5f)/scale-0.5f;
        const int ix=int(std::floor(u));
        for(unsigned c=0;c<4;++c) {
            std::array<float,4> p;
            for(int col=-1;col<=2;++col) {
                const int sx=std::clamp(ix+col,0,int(src.width)-1);
                p[col+1]=src.rgba[(size_t(y)*src.width+sx)*4+c];
            }
            horizontal[(size_t(y)*out.width+x)*4+c]=interpolate(p[0],p[1],p[2],p[3],u-ix);
        }
    }
    for(uint32_t y=0;y<out.height;++y) for(uint32_t x=0;x<out.width;++x) {
        if (x == 0 && checkpoint) checkpoint();
        const float v=(float(y)+0.5f)/scale-0.5f;
        const int iy=int(std::floor(v));
        for(unsigned c=0;c<4;++c) {
            std::array<float,4> rows;
            for(int row=-1;row<=2;++row) {
                const int sy=std::clamp(iy+row,0,int(src.height)-1);
                rows[row+1]=horizontal[(size_t(sy)*out.width+x)*4+c];
            }
            out.rgba[(size_t(y)*out.width+x)*4+c]=uint8_t(std::lround(
                interpolate(rows[0],rows[1],rows[2],rows[3],v-iy)));
        }
    }
    return out;
}

Image filter_surface(const Image& src, uint32_t texel_scale, const std::function<void()>& checkpoint) {
    validate(src);
    if (!texel_scale || texel_scale > 16 ||
        src.width % texel_scale || src.height % texel_scale)
        throw std::runtime_error("Invalid surface filter texel scale");
    Image out = src;
    const int radius = int(texel_scale);
    const uint32_t border = 2 * texel_scale;
    std::vector<uint8_t> luminance(size_t(src.width) * src.height);
    for (size_t p = 0; p < luminance.size(); ++p) {
        const auto i = p * 4;
        luminance[p] = uint8_t((54 * int(src.rgba[i]) + 183 * int(src.rgba[i+1]) +
            19 * int(src.rgba[i+2]) + 128) / 256);
    }
    const auto luma = [&](uint32_t x, uint32_t y) {
        return int(luminance[size_t(y) * src.width + x]);
    };
    const auto sample_luma = [&](float x, float y) {
        const auto ix = uint32_t(x), iy = uint32_t(y);
        const float u = x - ix, v = y - iy;
        return (1-v) * ((1-u)*luma(ix,iy) + u*luma(ix+1,iy)) +
            v * ((1-u)*luma(ix,iy+1) + u*luma(ix+1,iy+1));
    };
    for (uint32_t y = border; y + border < src.height; ++y)
        for (uint32_t x = border; x + border < src.width; ++x) {
            if (x == border && checkpoint) checkpoint();
            const int centre = luma(x, y);
            const float gx = float(luma(x+radius,y)-luma(x-radius,y));
            const float gy = float(luma(x,y+radius)-luma(x,y-radius));
            const float length = std::max(1.0f, std::sqrt(gx*gx+gy*gy));
            const float tx = -gy/length, ty = gx/length;
            float residual = 0;
            int total = 0;
            for (int d = -radius; d <= radius; ++d) {
                const float difference = sample_luma(float(x)+tx*d,float(y)+ty*d)-centre;
                const int weight = radius+1-std::abs(d);
                residual += weight*difference;
                total += weight;
            }
            int delta = int(std::lround(residual/total));
            const auto distance = std::min({x, y, src.width-1-x, src.height-1-y});
            const int limit = std::min(8U, distance - border + 1);
            int low = -limit, high = limit;
            const auto i = (size_t(y) * src.width + x) * 4;
            for (unsigned c = 0; c < 3; ++c) {
                low = std::max(low, -int(src.rgba[i+c]));
                high = std::min(high, 255 - int(src.rgba[i+c]));
            }
            delta = std::clamp(delta, low, high);
            for (unsigned c = 0; c < 3; ++c) out.rgba[i+c] = uint8_t(int(src.rgba[i+c]) + delta);
        }
    return out;
}

Bytes make_dds(const Image& image) {
    validate(image);
    uint32_t levels=1;
    for(uint32_t w=image.width,h=image.height;w>1||h>1;++levels) {w=std::max(1U,w/2);h=std::max(1U,h/2);}
    Bytes out(148,0);
    put(out,0,0x20534444);put(out,4,124);put(out,8,0x2100F);
    put(out,12,image.height);put(out,16,image.width);put(out,20,image.width*4);put(out,28,levels);
    put(out,76,32);put(out,80,4);put(out,84,0x30315844);put(out,108,0x401008);
    put(out,128,28);put(out,132,3);put(out,140,1); // R8G8B8A8_UNORM, 2D, one layer.
    Image level=image;
    for(;;) {
        out.insert(out.end(),level.rgba.begin(),level.rgba.end());
        if(level.width==1 && level.height==1) break;
        level=mip(level);
    }
    return out;
}

}
