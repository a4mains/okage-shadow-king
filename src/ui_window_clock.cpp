#include "okage/ui_window.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON

namespace okage {
namespace {
void check(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
std::int32_t signed_word(std::uint32_t value) {return __builtin_bit_cast(std::int32_t, (value));}
std::uint32_t add_wrap(std::uint32_t a,std::uint32_t b) {return a+b;}
std::uint32_t negate_wrap(std::uint32_t value) {return 0u-value;}
bool supported_float(std::uint32_t bits) {
    const auto exponent=bits&0x7f800000u;
    const auto fraction=bits&0x007fffffu;
    return exponent!=0x7f800000u&&(exponent!=0||fraction==0);
}
void require_float(std::uint32_t bits) {
    check(supported_float(bits),"UI resource clock does not support nonfinite or subnormal float state");
}
float as_float(std::uint32_t bits) {return __builtin_bit_cast(float, (bits));}
float add_single(float a,float b) {
    check(std::feclearexcept(FE_OVERFLOW)==0,"Cannot clear UI clock overflow flag");
    volatile float left=a,right=b;
    volatile float result=left+right;
    check((std::fetestexcept(FE_OVERFLOW)&FE_OVERFLOW)==0,
          "UI resource clock floating addition overflow is unsupported");
    require_float(__builtin_bit_cast(std::uint32_t, (float(result))));
    return result;
}
float signed_to_single(std::int32_t value) {
    volatile std::int32_t input=value;
    volatile float result=static_cast<float>(input);
    return result;
}
class TowardZero {
public:
    TowardZero() {
        check(std::fegetenv(&saved_)==0,"Cannot save UI clock floating environment");
        if(std::fesetround(FE_TOWARDZERO)!=0) {
            std::fesetenv(&saved_);
            throw std::runtime_error("Cannot set UI clock toward-zero rounding");
        }
    }
    TowardZero(const TowardZero&)=delete;
    TowardZero& operator=(const TowardZero&)=delete;
    ~TowardZero() {std::fesetenv(&saved_);}
private:
    std::fenv_t saved_{};
};
}

void UiWindow::advance_clock() {
    auto flags=word(0x1900);
    if((flags&0x8000u)==0||(word(0x25c)&0xffu)==0)return;

    const auto channelIndex=signed_word(word(0x5b0));
    constexpr std::size_t channelSlots=32;
    check(channelIndex>=0&&std::size_t(channelIndex)<channelSlots,
          "UI resource clock transform channel index is outside the 32-slot table");
    const auto channelPointer=word(0x5c8+std::size_t(channelIndex)*4);
    const auto channelStepBits=word(0x5c0);
    if(channelPointer!=0&&as_float(channelStepBits)!=0.0f)
        throw std::runtime_error("UI resource clock active transform channels are unsupported");

    const auto historyCount=signed_word(word(0x4f0));
    check(historyCount>=0&&historyCount<=15,"UI resource clock history count is outside 0..15");

    for(unsigned i=0;i<4;++i) {
        require_float(word(0x290+i*4));
        require_float(word(0x2a0+i*4));
        require_float(word(0x2b0+i*4));
        require_float(word(0x2c0+i*4));
    }
    const auto opacityDelta=word(0x27c);
    if(as_float(opacityDelta)!=0.0f) {
        require_float(opacityDelta);
        require_float(word(0x270));
        require_float(word(0x274));
        require_float(word(0x278));
    }
    const bool updateReveal=(flags&0x800u)==0&&(flags&0x100u)==0&&(flags&0x10u)==0;
    if(updateReveal) {
        require_float(word(0x264));
        require_float(word(0x268));
        require_float(word(0x26c));
    }

    TowardZero rounding;

    // 199808: one ADD.S per lane; strict lower test, then strict upper test.
    for(unsigned i=0;i<4;++i) {
        const auto currentAt=0x290+i*4,upperAt=0x2a0+i*4;
        const auto lowerAt=0x2b0+i*4,stepAt=0x2c0+i*4;
        const float current=as_float(word(currentAt));
        const float upper=as_float(word(upperAt));
        const float lower=as_float(word(lowerAt));
        const float step=as_float(word(stepAt));
        float result=add_single(current,step);
        put(currentAt,__builtin_bit_cast(std::uint32_t, (result)));
        if(result<lower) {
            if(step<0.0f) {
                result=lower;
            }
        }
        if(upper<result&&step>0.0f)
            result=upper;
        require_float(__builtin_bit_cast(std::uint32_t, (result)));
        put(currentAt,__builtin_bit_cast(std::uint32_t, (result)));
    }

    // 1998a8: equality at a bound keeps the increment for the next tick.
    if(as_float(opacityDelta)!=0.0f) {
        const float result=add_single(as_float(word(0x270)),as_float(opacityDelta));
        if(as_float(opacityDelta)>0.0f&&as_float(word(0x274))<result) {
            require_float(word(0x274));
            put(0x27c,0);
            put(0x270,word(0x274));
        } else if(as_float(opacityDelta)<0.0f&&result<as_float(word(0x278))) {
            require_float(word(0x278));
            put(0x27c,0);
            put(0x270,word(0x278));
        } else {
            require_float(__builtin_bit_cast(std::uint32_t, (result)));
            put(0x270,__builtin_bit_cast(std::uint32_t, (result)));
        }
    }

    // 1980f8 history: only XYZ lanes in each 16-byte entry are copied/written.
    auto count=historyCount;
    if(count==15) {
        for(unsigned i=0;i<14;++i) {
            const auto delta=i*0x10;
            for(unsigned lane=0;lane<3;++lane) {
                put(0x400+delta+lane*4,word(0x410+delta+lane*4));
                put(0x300+delta+lane*4,word(0x310+delta+lane*4));
            }
        }
        count=14;
        put(0x4f0,14);
    }
    for(unsigned lane=0;lane<3;++lane) {
        put(0x2f0+unsigned(count)*0x10+lane*4,word(0x520+lane*4));
        put(0x3f0+unsigned(count)*0x10+lane*4,word(0x550+lane*4));
    }
    put(0x4f0,add_wrap(std::uint32_t(count),1));

    const auto advanceCounters=[&]() {
        const auto countdown=word(0xcd8);
        if(countdown!=0) {
            put(0xcd8,add_wrap(countdown,0xffffffffu));
            return false;
        }
        const auto counter=add_wrap(word(0xcd0),1);
        put(0xcd0,counter);
        const auto periodBits=word(0xcd4);
        const auto counterSigned=signed_word(counter),period=signed_word(periodBits);
        if(period<counterSigned) {
            put(0xcd0,0);
            const auto increment=period<0?negate_wrap(periodBits):1u;
            put(0xce8,add_wrap(word(0xce8),increment));
        }
        return true;
    };
    bool dirtyUpdate=false;
    if(updateReveal) {
        const float target=signed_to_single(signed_word(word(0x260)));
        const float current=as_float(word(0x264));
        if(current<target) {
            const auto revealStepBits=word(0x268),revealControlBits=word(0x26c);
            const float revealStep=as_float(revealStepBits);
            if(revealStep!=0.0f&&as_float(revealControlBits)!=0.0f) {
                float result=add_single(current,revealStep);
                const bool clamp=target<result;
                require_float(__builtin_bit_cast(std::uint32_t, (clamp?target:result)));
                put(0x264,__builtin_bit_cast(std::uint32_t, (result)));
                if(clamp)put(0x264,__builtin_bit_cast(std::uint32_t, (target)));
                dirtyUpdate=true;
            } else {
                dirtyUpdate=advanceCounters();
            }
        } else {
            dirtyUpdate=advanceCounters();
        }
    }
    if(dirtyUpdate&&(flags&0x20u)==0)flags|=1u;
    if(flags&8u)flags&=~8u;
    if(flags!=word(0x1900))put(0x1900,flags);
}

} // namespace okage
