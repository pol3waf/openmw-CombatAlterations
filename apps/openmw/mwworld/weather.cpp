#define _USE_MATH_DEFINES
#include <cmath>

#include "weather.hpp"

#include <components/misc/rng.hpp>

#include <components/esm/esmwriter.hpp>
#include <components/esm/weatherstate.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/soundmanager.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwsound/sound.hpp"

#include "../mwrender/renderingmanager.hpp"
#include "../mwrender/sky.hpp"

#include "player.hpp"
#include "esmstore.hpp"
#include "fallback.hpp"
#include "cellstore.hpp"

using namespace MWWorld;

namespace
{
    float lerp (float x, float y, float factor)
    {
        return x * (1-factor) + y * factor;
    }

    osg::Vec4f lerp (const osg::Vec4f& x, const osg::Vec4f& y, float factor)
    {
        return x * (1-factor) + y * factor;
    }
}

Weather::Weather(const std::string& name,
                 const MWWorld::Fallback& fallback,
                 float stormWindSpeed,
                 float rainSpeed,
                 const std::string& ambientLoopSoundID,
                 const std::string& particleEffect)
    : mCloudTexture(fallback.getFallbackString("Weather_" + name + "_Cloud_Texture"))
    , mSkySunriseColor(fallback.getFallbackColour("Weather_" + name +"_Sky_Sunrise_Color"))
    , mSkyDayColor(fallback.getFallbackColour("Weather_" + name + "_Sky_Day_Color"))
    , mSkySunsetColor(fallback.getFallbackColour("Weather_" + name + "_Sky_Sunset_Color"))
    , mSkyNightColor(fallback.getFallbackColour("Weather_" + name + "_Sky_Night_Color"))
    , mFogSunriseColor(fallback.getFallbackColour("Weather_" + name + "_Fog_Sunrise_Color"))
    , mFogDayColor(fallback.getFallbackColour("Weather_" + name + "_Fog_Day_Color"))
    , mFogSunsetColor(fallback.getFallbackColour("Weather_" + name + "_Fog_Sunset_Color"))
    , mFogNightColor(fallback.getFallbackColour("Weather_" + name + "_Fog_Night_Color"))
    , mAmbientSunriseColor(fallback.getFallbackColour("Weather_" + name + "_Ambient_Sunrise_Color"))
    , mAmbientDayColor(fallback.getFallbackColour("Weather_" + name + "_Ambient_Day_Color"))
    , mAmbientSunsetColor(fallback.getFallbackColour("Weather_" + name + "_Ambient_Sunset_Color"))
    , mAmbientNightColor(fallback.getFallbackColour("Weather_" + name + "_Ambient_Night_Color"))
    , mSunSunriseColor(fallback.getFallbackColour("Weather_" + name + "_Sun_Sunrise_Color"))
    , mSunDayColor(fallback.getFallbackColour("Weather_" + name + "_Sun_Day_Color"))
    , mSunSunsetColor(fallback.getFallbackColour("Weather_" + name + "_Sun_Sunset_Color"))
    , mSunNightColor(fallback.getFallbackColour("Weather_" + name + "_Sun_Night_Color"))
    , mLandFogDayDepth(fallback.getFallbackFloat("Weather_" + name + "_Land_Fog_Day_Depth"))
    , mLandFogNightDepth(fallback.getFallbackFloat("Weather_" + name + "_Land_Fog_Night_Depth"))
    , mSunDiscSunsetColor(fallback.getFallbackColour("Weather_" + name + "_Sun_Disc_Sunset_Color"))
    , mWindSpeed(fallback.getFallbackFloat("Weather_" + name + "_Wind_Speed"))
    , mCloudSpeed(fallback.getFallbackFloat("Weather_" + name + "_Cloud_Speed"))
    , mGlareView(fallback.getFallbackFloat("Weather_" + name + "_Glare_View"))
    , mAmbientLoopSoundID(ambientLoopSoundID)
    , mIsStorm(mWindSpeed > stormWindSpeed)
    , mRainSpeed(rainSpeed)
    , mRainFrequency(fallback.getFallbackFloat("Weather_" + name + "_Rain_Entrance_Speed"))
    , mParticleEffect(particleEffect)
    , mRainEffect(fallback.getFallbackBool("Weather_" + name + "_Using_Precip") ? "meshes\\raindrop.nif" : "")
    , mTransitionDelta(fallback.getFallbackFloat("Weather_" + name + "_Transition_Delta"))
    , mCloudsMaximumPercent(fallback.getFallbackFloat("Weather_" + name + "_Clouds_Maximum_Percent"))
{
/*
Unhandled:
Rain Diameter=600 ?
Rain Height Min=200 ?
Rain Height Max=700 ?
Rain Threshold=0.6 ?
Max Raindrops=650 ?
*/
}

float Weather::transitionSeconds() const
{
   // This formula is reversed from Morrowind by observing different Transition Delta values with Clouds
   // Maximum Percent set to 1.0, and watching for when the light from the sun was no longer visible.
   static const float deltasPerHour = 0.00835;
   return (deltasPerHour / mTransitionDelta) * 60.0f * 60.0f;
}

float Weather::cloudBlendFactor(float transitionRatio) const
{
    // Clouds Maximum Percent affects how quickly the sky transitions from one sky texture to the next.
    return transitionRatio / mCloudsMaximumPercent;
}

MoonModel::MoonModel(const std::string& name, const MWWorld::Fallback& fallback)
  : mFadeInStart(fallback.getFallbackFloat("Moons_" + name + "_Fade_In_Start"))
  , mFadeInFinish(fallback.getFallbackFloat("Moons_" + name + "_Fade_In_Finish"))
  , mFadeOutStart(fallback.getFallbackFloat("Moons_" + name + "_Fade_Out_Start"))
  , mFadeOutFinish(fallback.getFallbackFloat("Moons_" + name + "_Fade_Out_Finish"))
  , mAxisOffset(fallback.getFallbackFloat("Moons_" + name + "_Axis_Offset"))
  , mSpeed(fallback.getFallbackFloat("Moons_" + name + "_Speed"))
  , mDailyIncrement(fallback.getFallbackFloat("Moons_" + name + "_Daily_Increment"))
  , mFadeStartAngle(fallback.getFallbackFloat("Moons_" + name + "_Fade_Start_Angle"))
  , mFadeEndAngle(fallback.getFallbackFloat("Moons_" + name + "_Fade_End_Angle"))
  , mMoonShadowEarlyFadeAngle(fallback.getFallbackFloat("Moons_" + name + "_Moon_Shadow_Early_Fade_Angle"))
{
    // Morrowind appears to have a minimum speed in order to avoid situations where the moon couldn't conceivably
    // complete a rotation in a single 24 hour period. The value of 180/23 was deduced from reverse engineering.
    mSpeed = std::min(mSpeed, 180.0f / 23.0f);
}

MWRender::MoonState MoonModel::calculateState(unsigned int daysPassed, float gameHour) const
{
    float rotationFromHorizon = angle(daysPassed, gameHour);
    MWRender::MoonState state =
    {
        rotationFromHorizon,
        mAxisOffset, // Reverse engineered from Morrowind's scene graph rotation matrices.
        static_cast<MWRender::MoonState::Phase>(phase(daysPassed, gameHour)),
        shadowBlend(rotationFromHorizon),
        earlyMoonShadowAlpha(rotationFromHorizon) * hourlyAlpha(gameHour)
    };

    return state;
}

inline float MoonModel::angle(unsigned int daysPassed, float gameHour) const
{
    // Morrowind's moons start travel on one side of the horizon (let's call it H-rise) and travel 180 degrees to the
    // opposite horizon (let's call it H-set). Upon reaching H-set, they reset to H-rise until the next moon rise.

    // When calculating the angle of the moon, several cases have to be taken into account:
    // 1. Moon rises and then sets in one day.
    // 2. Moon sets and doesn't rise in one day (occurs when the moon rise hour is >= 24).
    // 3. Moon sets and then rises in one day.
    float moonRiseHourToday = moonRiseHour(daysPassed);
    float moonRiseAngleToday = 0;

    if(gameHour < moonRiseHourToday)
    {
        float moonRiseHourYesterday = moonRiseHour(daysPassed - 1);
        if(moonRiseHourYesterday < 24)
        {
            float moonRiseAngleYesterday = rotation(24 - moonRiseHourYesterday);
            if(moonRiseAngleYesterday < 180)
            {
                // The moon rose but did not set yesterday, so accumulate yesterday's angle with how much we've travelled today.
                moonRiseAngleToday = rotation(gameHour) + moonRiseAngleYesterday;
            }
        }
    }
    else
    {
        moonRiseAngleToday = rotation(gameHour - moonRiseHourToday);
    }

    if(moonRiseAngleToday >= 180)
    {
        // The moon set today, reset the angle to the horizon.
        moonRiseAngleToday = 0;
    }

    return moonRiseAngleToday;
}

inline float MoonModel::moonRiseHour(unsigned int daysPassed) const
{
    // This arises from the start date of 16 Last Seed, 427
    // TODO: Find an alternate formula that doesn't rely on this day being fixed.
    static const unsigned int startDay = 16;

    // This odd formula arises from the fact that on 16 Last Seed, 17 increments have occurred, meaning
    // that upon starting a new game, it must only calculate the moon phase as far back as 1 Last Seed.
    // Note that we don't modulo after adding the latest daily increment because other calculations need to
    // know if doing so would cause the moon rise to be postponed until the next day (which happens when
    // the moon rise hour is >= 24 in Morrowind).
    return mDailyIncrement + std::fmod((daysPassed - 1 + startDay) * mDailyIncrement, 24.0f);
}

inline float MoonModel::rotation(float hours) const
{
    // 15 degrees per hour was reverse engineered from the rotation matrices of the Morrowind scene graph.
    // Note that this correlates to 360 / 24, which is a full rotation every 24 hours, so speed is a measure
    // of whole rotations that could be completed in a day.
    return 15.0f * mSpeed * hours;
}

inline unsigned int MoonModel::phase(unsigned int daysPassed, float gameHour) const
{
    // Morrowind starts with a full moon on 16 Last Seed and then begins to wane 17 Last Seed, working on 3 day phase cycle.
    // Note: this is an internal helper, and as such we don't want to return MWRender::MoonState::Phase since we can't
    // forward declare it (C++11 strongly typed enums solve this).

    // If the moon didn't rise yet today, use yesterday's moon phase.
    if(gameHour < moonRiseHour(daysPassed))
        return (daysPassed / 3) % 8;
    else
        return ((daysPassed + 1) / 3) % 8;
}

inline float MoonModel::shadowBlend(float angle) const
{
    // The Fade End Angle and Fade Start Angle describe a region where the moon transitions from a solid disk
    // that is roughly the color of the sky, to a textured surface.
    // Depending on the current angle, the following values describe the ratio between the textured moon
    // and the solid disk:
    // 1. From Fade End Angle 1 to Fade Start Angle 1 (during moon rise): 0..1
    // 2. From Fade Start Angle 1 to Fade Start Angle 2 (between moon rise and moon set): 1 (textured)
    // 3. From Fade Start Angle 2 to Fade End Angle 2 (during moon set): 1..0
    // 4. From Fade End Angle 2 to Fade End Angle 1 (between moon set and moon rise): 0 (solid disk)
    float fadeAngle = mFadeStartAngle - mFadeEndAngle;
    float fadeEndAngle2 = 180.0f - mFadeEndAngle;
    float fadeStartAngle2 = 180.0f - mFadeStartAngle;
    if((angle >= mFadeEndAngle) && (angle < mFadeStartAngle))
        return (angle - mFadeEndAngle) / fadeAngle;
    else if((angle >= mFadeStartAngle) && (angle < fadeStartAngle2))
        return 1.0f;
    else if((angle >= fadeStartAngle2) && (angle < fadeEndAngle2))
        return (fadeEndAngle2 - angle) / fadeAngle;
    else
        return 0.0f;
}

inline float MoonModel::hourlyAlpha(float gameHour) const
{
    // The Fade Out Start / Finish and Fade In Start / Finish describe the hours at which the moon
    // appears and disappears.
    // Depending on the current hour, the following values describe how transparent the moon is.
    // 1. From Fade Out Start to Fade Out Finish: 1..0
    // 2. From Fade Out Finish to Fade In Start: 0 (transparent)
    // 3. From Fade In Start to Fade In Finish: 0..1
    // 4. From Fade In Finish to Fade Out Start: 1 (solid)
    if((gameHour >= mFadeOutStart) && (gameHour < mFadeOutFinish))
        return (mFadeOutFinish - gameHour) / (mFadeOutFinish - mFadeOutStart);
    else if((gameHour >= mFadeOutFinish) && (gameHour < mFadeInStart))
        return 0.0f;
    else if((gameHour >= mFadeInStart) && (gameHour < mFadeInFinish))
        return (gameHour - mFadeInStart) / (mFadeInFinish - mFadeInStart);
    else
        return 1.0f;
}

inline float MoonModel::earlyMoonShadowAlpha(float angle) const
{
    // The Moon Shadow Early Fade Angle describes an arc relative to Fade End Angle.
    // Depending on the current angle, the following values describe how transparent the moon is.
    // 1. From Moon Shadow Early Fade Angle 1 to Fade End Angle 1 (during moon rise): 0..1
    // 2. From Fade End Angle 1 to Fade End Angle 2 (between moon rise and moon set): 1 (solid)
    // 3. From Fade End Angle 2 to Moon Shadow Early Fade Angle 2 (during moon set): 1..0
    // 4. From Moon Shadow Early Fade Angle 2 to Moon Shadow Early Fade Angle 1: 0 (transparent)
    float moonShadowEarlyFadeAngle1 = mFadeEndAngle - mMoonShadowEarlyFadeAngle;
    float fadeEndAngle2 = 180.0f - mFadeEndAngle;
    float moonShadowEarlyFadeAngle2 = fadeEndAngle2 + mMoonShadowEarlyFadeAngle;
    if((angle >= moonShadowEarlyFadeAngle1) && (angle < mFadeEndAngle))
        return (angle - moonShadowEarlyFadeAngle1) / mMoonShadowEarlyFadeAngle;
    else if((angle >= mFadeEndAngle) && (angle < fadeEndAngle2))
        return 1.0f;
    else if((angle >= fadeEndAngle2) && (angle < moonShadowEarlyFadeAngle2))
        return (moonShadowEarlyFadeAngle2 - angle) / mMoonShadowEarlyFadeAngle;
    else
        return 0.0f;
}

WeatherManager::WeatherManager(MWRender::RenderingManager* rendering, MWWorld::Fallback* fallback, MWWorld::ESMStore* store) :
     mHour(14), mWindSpeed(0.f), mIsStorm(false), mStormDirection(0,1,0), mStore(store),
     mRendering(rendering), mCurrentWeather("clear"), mNextWeather(""), mFirstUpdate(true),
     mRemainingTransitionTime(0), mThunderFlash(0), mThunderChance(0), mThunderChanceNeeded(50),
     mTimePassed(0), mWeatherUpdateTime(0), mThunderSoundDelay(0),
     mMasser("Masser", *fallback), mSecunda("Secunda", *fallback)
{
    //Globals
    mThunderSoundID0 = fallback->getFallbackString("Weather_Thunderstorm_Thunder_Sound_ID_0");
    mThunderSoundID1 = fallback->getFallbackString("Weather_Thunderstorm_Thunder_Sound_ID_1");
    mThunderSoundID2 = fallback->getFallbackString("Weather_Thunderstorm_Thunder_Sound_ID_2");
    mThunderSoundID3 = fallback->getFallbackString("Weather_Thunderstorm_Thunder_Sound_ID_3");
    mSunriseTime = fallback->getFallbackFloat("Weather_Sunrise_Time");
    mSunsetTime = fallback->getFallbackFloat("Weather_Sunset_Time");
    mSunriseDuration = fallback->getFallbackFloat("Weather_Sunrise_Duration");
    mSunsetDuration = fallback->getFallbackFloat("Weather_Sunset_Duration");
    mHoursBetweenWeatherChanges = fallback->getFallbackFloat("Weather_Hours_Between_Weather_Changes");
    mWeatherUpdateTime = mHoursBetweenWeatherChanges * 3600;
    mThunderFrequency = fallback->getFallbackFloat("Weather_Thunderstorm_Thunder_Frequency");
    mThunderThreshold = fallback->getFallbackFloat("Weather_Thunderstorm_Thunder_Threshold");
    mThunderSoundDelay = 0.25;

    mRainSpeed = fallback->getFallbackFloat("Weather_Precip_Gravity");

    //Some useful values
    /* TODO: Use pre-sunrise_time, pre-sunset_time,
     * post-sunrise_time, and post-sunset_time to better
     * describe sunrise/sunset time.
     * These values are fallbacks attached to weather.
     */
    mNightStart = mSunsetTime + mSunsetDuration;
    mNightEnd = mSunriseTime - 0.5f;
    mDayStart = mSunriseTime + mSunriseDuration;
    mDayEnd = mSunsetTime;

    addWeather("Clear", *fallback);
    addWeather("Cloudy", *fallback);
    addWeather("Foggy", *fallback);
    addWeather("Overcast", *fallback);
    addWeather("Rain", *fallback, "rain");
    addWeather("Thunderstorm", *fallback, "rain heavy");
    addWeather("Ashstorm", *fallback, "ashstorm", "meshes\\ashcloud.nif");
    addWeather("Blight", *fallback, "blight", "meshes\\blightcloud.nif");
    addWeather("Snow", *fallback, "", "meshes\\snow.nif");
    addWeather("Blizzard", *fallback, "BM Blizzard", "meshes\\blizzard.nif");
}

WeatherManager::~WeatherManager()
{
    stopSounds();
}

void WeatherManager::setWeather(const std::string& weather, bool instant)
{
    if (weather == mCurrentWeather && mNextWeather == "")
    {
        mFirstUpdate = false;
        return;
    }

    if (instant || mFirstUpdate)
    {
        mNextWeather = "";
        mCurrentWeather = weather;
    }
    else
    {
        if (mNextWeather != "")
        {
            // transition more than 50% finished?
            if (mRemainingTransitionTime / (findWeather(mCurrentWeather).transitionSeconds()) <= 0.5)
                mCurrentWeather = mNextWeather;
        }

        mNextWeather = weather;
        mRemainingTransitionTime = findWeather(mCurrentWeather).transitionSeconds();
    }
    mFirstUpdate = false;
}

void WeatherManager::setResult(const std::string& weatherType)
{
    const Weather& current = findWeather(weatherType);

    mResult.mCloudTexture = current.mCloudTexture;
    mResult.mCloudBlendFactor = 0;
    mResult.mWindSpeed = current.mWindSpeed;
    mResult.mCloudSpeed = current.mCloudSpeed;
    mResult.mGlareView = current.mGlareView;
    mResult.mAmbientLoopSoundID = current.mAmbientLoopSoundID;
    mResult.mAmbientSoundVolume = 1.f;
    mResult.mEffectFade = 1.f;
    mResult.mSunColor = current.mSunDiscSunsetColor;

    mResult.mIsStorm = current.mIsStorm;

    mResult.mRainSpeed = current.mRainSpeed;
    mResult.mRainFrequency = current.mRainFrequency;

    mResult.mParticleEffect = current.mParticleEffect;
    mResult.mRainEffect = current.mRainEffect;

    mResult.mNight = (mHour < mSunriseTime || mHour > mNightStart - 1);

    mResult.mFogDepth = mResult.mNight ? current.mLandFogNightDepth : current.mLandFogDayDepth;

    // night
    if (mHour <= mNightEnd || mHour >= mNightStart + 1)
    {
        mResult.mFogColor = current.mFogNightColor;
        mResult.mAmbientColor = current.mAmbientNightColor;
        mResult.mSunColor = current.mSunNightColor;
        mResult.mSkyColor = current.mSkyNightColor;
        mResult.mNightFade = 1.f;
    }

    // sunrise
    else if (mHour >= mNightEnd && mHour <= mDayStart + 1)
    {
        if (mHour <= mSunriseTime)
        {
            // fade in
            float advance = mSunriseTime - mHour;
            float factor = advance / 0.5f;
            mResult.mFogColor = lerp(current.mFogSunriseColor, current.mFogNightColor, factor);
            mResult.mAmbientColor = lerp(current.mAmbientSunriseColor, current.mAmbientNightColor, factor);
            mResult.mSunColor = lerp(current.mSunSunriseColor, current.mSunNightColor, factor);
            mResult.mSkyColor = lerp(current.mSkySunriseColor, current.mSkyNightColor, factor);
            mResult.mNightFade = factor;
        }
        else //if (mHour >= 6)
        {
            // fade out
            float advance = mHour - mSunriseTime;
            float factor = advance / 3.f;
            mResult.mFogColor = lerp(current.mFogSunriseColor, current.mFogDayColor, factor);
            mResult.mAmbientColor = lerp(current.mAmbientSunriseColor, current.mAmbientDayColor, factor);
            mResult.mSunColor = lerp(current.mSunSunriseColor, current.mSunDayColor, factor);
            mResult.mSkyColor = lerp(current.mSkySunriseColor, current.mSkyDayColor, factor);
        }
    }

    // day
    else if (mHour >= mDayStart + 1 && mHour <= mDayEnd - 1)
    {
        mResult.mFogColor = current.mFogDayColor;
        mResult.mAmbientColor = current.mAmbientDayColor;
        mResult.mSunColor = current.mSunDayColor;
        mResult.mSkyColor = current.mSkyDayColor;
    }

    // sunset
    else if (mHour >= mDayEnd - 1 && mHour <= mNightStart + 1)
    {
        if (mHour <= mDayEnd + 1)
        {
            // fade in
            float advance = (mDayEnd + 1) - mHour;
            float factor = (advance / 2);
            mResult.mFogColor = lerp(current.mFogSunsetColor, current.mFogDayColor, factor);
            mResult.mAmbientColor = lerp(current.mAmbientSunsetColor, current.mAmbientDayColor, factor);
            mResult.mSunColor = lerp(current.mSunSunsetColor, current.mSunDayColor, factor);
            mResult.mSkyColor = lerp(current.mSkySunsetColor, current.mSkyDayColor, factor);
        }
        else //if (mHour >= 19)
        {
            // fade out
            float advance = mHour - (mDayEnd + 1);
            float factor = advance / 2.f;
            mResult.mFogColor = lerp(current.mFogSunsetColor, current.mFogNightColor, factor);
            mResult.mAmbientColor = lerp(current.mAmbientSunsetColor, current.mAmbientNightColor, factor);
            mResult.mSunColor = lerp(current.mSunSunsetColor, current.mSunNightColor, factor);
            mResult.mSkyColor = lerp(current.mSkySunsetColor, current.mSkyNightColor, factor);
            mResult.mNightFade = factor;
        }
    }
}

void WeatherManager::transition(float factor)
{
    setResult(mCurrentWeather);
    const MWRender::WeatherResult current = mResult;
    setResult(mNextWeather);
    const MWRender::WeatherResult other = mResult;

    const Weather& nextWeather = findWeather(mNextWeather);

    mResult.mCloudTexture = current.mCloudTexture;
    mResult.mNextCloudTexture = other.mCloudTexture;
    mResult.mCloudBlendFactor = nextWeather.cloudBlendFactor(factor);

    mResult.mFogColor = lerp(current.mFogColor, other.mFogColor, factor);
    mResult.mSunColor = lerp(current.mSunColor, other.mSunColor, factor);
    mResult.mSkyColor = lerp(current.mSkyColor, other.mSkyColor, factor);

    mResult.mAmbientColor = lerp(current.mAmbientColor, other.mAmbientColor, factor);
    mResult.mSunDiscColor = lerp(current.mSunDiscColor, other.mSunDiscColor, factor);
    mResult.mFogDepth = lerp(current.mFogDepth, other.mFogDepth, factor);
    mResult.mWindSpeed = lerp(current.mWindSpeed, other.mWindSpeed, factor);
    mResult.mCloudSpeed = lerp(current.mCloudSpeed, other.mCloudSpeed, factor);
    mResult.mGlareView = lerp(current.mGlareView, other.mGlareView, factor);
    mResult.mNightFade = lerp(current.mNightFade, other.mNightFade, factor);

    mResult.mNight = current.mNight;

    if (factor < 0.5)
    {
        mResult.mIsStorm = current.mIsStorm;
        mResult.mParticleEffect = current.mParticleEffect;
        mResult.mRainEffect = current.mRainEffect;
        mResult.mParticleEffect = current.mParticleEffect;
        mResult.mRainSpeed = current.mRainSpeed;
        mResult.mRainFrequency = current.mRainFrequency;
        mResult.mAmbientSoundVolume = 1-(factor*2);
        mResult.mEffectFade = mResult.mAmbientSoundVolume;
        mResult.mAmbientLoopSoundID = current.mAmbientLoopSoundID;
    }
    else
    {
        mResult.mIsStorm = other.mIsStorm;
        mResult.mParticleEffect = other.mParticleEffect;
        mResult.mRainEffect = other.mRainEffect;
        mResult.mParticleEffect = other.mParticleEffect;
        mResult.mRainSpeed = other.mRainSpeed;
        mResult.mRainFrequency = other.mRainFrequency;
        mResult.mAmbientSoundVolume = 2*(factor-0.5f);
        mResult.mEffectFade = mResult.mAmbientSoundVolume;
        mResult.mAmbientLoopSoundID = other.mAmbientLoopSoundID;
    }
}

void WeatherManager::update(float duration, bool paused)
{
    float timePassed = static_cast<float>(mTimePassed);
    mTimePassed = 0;

    mWeatherUpdateTime -= timePassed;

    MWBase::World* world = MWBase::Environment::get().getWorld();
    const bool exterior = (world->isCellExterior() || world->isCellQuasiExterior());
    if (!exterior)
    {
        mRendering->setSkyEnabled(false);
        //mRendering->getSkyManager()->setLightningStrength(0.f);
        stopSounds();
        return;
    }

    switchToNextWeather(false);

    if (mNextWeather != "")
    {
        mRemainingTransitionTime -= timePassed;
        if (mRemainingTransitionTime < 0)
        {
            mCurrentWeather = mNextWeather;
            mNextWeather = "";
        }
    }

    if (mNextWeather != "")
        transition(1 - (mRemainingTransitionTime / (findWeather(mCurrentWeather).transitionSeconds())));
    else
        setResult(mCurrentWeather);

    mWindSpeed = mResult.mWindSpeed;
    mIsStorm = mResult.mIsStorm;

    if (mIsStorm)
    {
        MWWorld::Ptr player = world->getPlayerPtr();
        osg::Vec3f playerPos (player.getRefData().getPosition().asVec3());
        osg::Vec3f redMountainPos (19950, 72032, 27831);

        mStormDirection = (playerPos - redMountainPos);
        mStormDirection.z() = 0;
        mStormDirection.normalize();
        mRendering->getSkyManager()->setStormDirection(mStormDirection);
    }

    mRendering->configureFog(mResult.mFogDepth, mResult.mFogColor);

    // disable sun during night
    if (mHour >= mNightStart || mHour <= mSunriseTime)
        mRendering->getSkyManager()->sunDisable();
    else
        mRendering->getSkyManager()->sunEnable();

    // Update the sun direction.  Run it east to west at a fixed angle from overhead.
    // The sun's speed at day and night may differ, since mSunriseTime and mNightStart
    // mark when the sun is level with the horizon.
    {
        // Shift times into a 24-hour window beginning at mSunriseTime...
        float adjustedHour = mHour;
        float adjustedNightStart = mNightStart;
        if ( mHour < mSunriseTime )
            adjustedHour += 24.f;
        if ( mNightStart < mSunriseTime )
            adjustedNightStart += 24.f;

        const bool is_night = adjustedHour >= adjustedNightStart;
        const float dayDuration = adjustedNightStart - mSunriseTime;
        const float nightDuration = 24.f - dayDuration;

        double theta;
        if ( !is_night ) {
            theta = M_PI * (adjustedHour - mSunriseTime) / dayDuration;
        } else {
            theta = M_PI * (adjustedHour - adjustedNightStart) / nightDuration;
        }

        osg::Vec3f final(
            static_cast<float>(cos(theta)),
            -0.268f, // approx tan( -15 degrees )
            static_cast<float>(sin(theta)));
        mRendering->setSunDirection( final * -1 );
    }

    TimeStamp time = MWBase::Environment::get().getWorld()->getTimeStamp();
    mRendering->getSkyManager()->setMasserState(mMasser.calculateState(time.getDay(), time.getHour()));
    mRendering->getSkyManager()->setSecundaState(mSecunda.calculateState(time.getDay(), time.getHour()));

    if (!paused)
    {
        if (mCurrentWeather == "thunderstorm" && mNextWeather == "")
        {
            if (mThunderFlash > 0)
            {
                // play the sound after a delay
                mThunderSoundDelay -= duration;
                if (mThunderSoundDelay <= 0)
                {
                    // pick a random sound
                    int sound = Misc::Rng::rollDice(4);
                    std::string* soundName = NULL;
                    if (sound == 0) soundName = &mThunderSoundID0;
                    else if (sound == 1) soundName = &mThunderSoundID1;
                    else if (sound == 2) soundName = &mThunderSoundID2;
                    else if (sound == 3) soundName = &mThunderSoundID3;
                    if (soundName)
                        MWBase::Environment::get().getSoundManager()->playSound(*soundName, 1.0, 1.0);
                    mThunderSoundDelay = 1000;
                }

                mThunderFlash -= duration;
                //if (mThunderFlash > 0)
                    //mRendering->getSkyManager()->setLightningStrength( mThunderFlash / mThunderThreshold );
                //else
                {
                    mThunderChanceNeeded = static_cast<float>(Misc::Rng::rollDice(100));
                    mThunderChance = 0;
                    //mRendering->getSkyManager()->setLightningStrength( 0.f );
                }
            }
            else
            {
                // no thunder active
                mThunderChance += duration*4; // chance increases by 4 percent every second
                if (mThunderChance >= mThunderChanceNeeded)
                {
                    mThunderFlash = mThunderThreshold;

                    //mRendering->getSkyManager()->setLightningStrength( mThunderFlash / mThunderThreshold );

                    mThunderSoundDelay = 0.25;
                }
            }
        }
        //else
            //mRendering->getSkyManager()->setLightningStrength(0.f);
    }
    

    mRendering->setAmbientColour(mResult.mAmbientColor);
    mRendering->setSunColour(mResult.mSunColor);

    mRendering->getSkyManager()->setWeather(mResult);

    // Play sounds
    if (mPlayingSoundID != mResult.mAmbientLoopSoundID)
    {
        stopSounds();
        if (!mResult.mAmbientLoopSoundID.empty())
            mAmbientSound = MWBase::Environment::get().getSoundManager()->playSound(mResult.mAmbientLoopSoundID, 1.0, 1.0, MWBase::SoundManager::Play_TypeSfx, MWBase::SoundManager::Play_Loop);

        mPlayingSoundID = mResult.mAmbientLoopSoundID;
    }
    if (mAmbientSound.get())
        mAmbientSound->setVolume(mResult.mAmbientSoundVolume);
}

void WeatherManager::stopSounds()
{
    if (mAmbientSound.get())
    {
        MWBase::Environment::get().getSoundManager()->stopSound(mAmbientSound);
        mAmbientSound.reset();
        mPlayingSoundID.clear();
    }
}

std::string WeatherManager::nextWeather(const ESM::Region* region) const
{
    std::vector<char> probability;

    RegionModMap::const_iterator iter = mRegionMods.find(Misc::StringUtils::lowerCase(region->mId));
    if(iter != mRegionMods.end())
        probability = iter->second;
    else
    {
        probability.reserve(10);
        probability.push_back(region->mData.mClear);
        probability.push_back(region->mData.mCloudy);
        probability.push_back(region->mData.mFoggy);
        probability.push_back(region->mData.mOvercast);
        probability.push_back(region->mData.mRain);
        probability.push_back(region->mData.mThunder);
        probability.push_back(region->mData.mAsh);
        probability.push_back(region->mData.mBlight);
        probability.push_back(region->mData.mA);
        probability.push_back(region->mData.mB);
    }

    /*
     * All probabilities must add to 100 (responsibility of the user).
     * If chances A and B has values 30 and 70 then by generating
     * 100 numbers 1..100, 30% will be lesser or equal 30 and
     * 70% will be greater than 30 (in theory).
     */

    int chance = Misc::Rng::rollDice(100) + 1; // 1..100
    int sum = 0;
    unsigned int i = 0;
    for (; i < probability.size(); ++i)
    {
        sum += probability[i];
        if (chance < sum)
            break;
    }

    switch (i)
    {
        case 1:
            return "cloudy";
        case 2:
            return "foggy";
        case 3:
            return "overcast";
        case 4:
            return "rain";
        case 5:
            return "thunderstorm";
        case 6:
            return "ashstorm";
        case 7:
            return "blight";
        case 8:
            return "snow";
        case 9:
            return "blizzard";
        default: // case 0
            return "clear";
    }
}

void WeatherManager::setHour(const float hour)
{
    mHour = hour;
}

unsigned int WeatherManager::getWeatherID() const
{
    // Source: http://www.uesp.net/wiki/Tes3Mod:GetCurrentWeather

    if (mCurrentWeather == "clear")
        return 0;
    else if (mCurrentWeather == "cloudy")
        return 1;
    else if (mCurrentWeather == "foggy")
        return 2;
    else if (mCurrentWeather == "overcast")
        return 3;
    else if (mCurrentWeather == "rain")
        return 4;
    else if (mCurrentWeather == "thunderstorm")
        return 5;
    else if (mCurrentWeather == "ashstorm")
        return 6;
    else if (mCurrentWeather == "blight")
        return 7;
    else if (mCurrentWeather == "snow")
        return 8;
    else if (mCurrentWeather == "blizzard")
        return 9;

    else
        return 0;
}

void WeatherManager::changeWeather(const std::string& region, const unsigned int id)
{
    // make sure this region exists
    MWBase::Environment::get().getWorld()->getStore().get<ESM::Region>().find(region);

    std::string weather;
    if (id==0)
        weather = "clear";
    else if (id==1)
        weather = "cloudy";
    else if (id==2)
        weather = "foggy";
    else if (id==3)
        weather = "overcast";
    else if (id==4)
        weather = "rain";
    else if (id==5)
        weather = "thunderstorm";
    else if (id==6)
        weather = "ashstorm";
    else if (id==7)
        weather = "blight";
    else if (id==8)
        weather = "snow";
    else if (id==9)
        weather = "blizzard";
    else
        weather = "clear";

    mRegionOverrides[Misc::StringUtils::lowerCase(region)] = weather;

    MWWorld::Ptr player = MWMechanics::getPlayer();
    if (player.isInCell())
    {
        std::string playerRegion = player.getCell()->getCell()->mRegion;
        if (Misc::StringUtils::ciEqual(region, playerRegion))
            setWeather(weather);
    }
}

void WeatherManager::modRegion(const std::string &regionid, const std::vector<char> &chances)
{
    mRegionMods[Misc::StringUtils::lowerCase(regionid)] = chances;
    // Start transitioning right away if the region no longer supports the current weather type
    unsigned int current = getWeatherID();
    if(current >= chances.size() || chances[current] == 0)
        mWeatherUpdateTime = 0.0f;
}

float WeatherManager::getWindSpeed() const
{
    return mWindSpeed;
}

bool WeatherManager::isDark() const
{
    bool exterior = (MWBase::Environment::get().getWorld()->isCellExterior()
                     || MWBase::Environment::get().getWorld()->isCellQuasiExterior());
    return exterior && (mHour < mSunriseTime || mHour > mNightStart - 1);
}

void WeatherManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
{
    ESM::WeatherState state;
    state.mHour = mHour;
    state.mWindSpeed = mWindSpeed;
    state.mCurrentWeather = mCurrentWeather;
    state.mNextWeather = mNextWeather;
    state.mCurrentRegion = mCurrentRegion;
    state.mFirstUpdate = mFirstUpdate;
    state.mRemainingTransitionTime = mRemainingTransitionTime;
    state.mTimePassed = mTimePassed;

    writer.startRecord(ESM::REC_WTHR);
    state.save(writer);
    writer.endRecord(ESM::REC_WTHR);
}

bool WeatherManager::readRecord(ESM::ESMReader& reader, uint32_t type)
{
    if(ESM::REC_WTHR == type)
    {
        // load first so that if it fails, we haven't accidentally reset the state below
        ESM::WeatherState state;
        state.load(reader);

        // swap in the loaded values now that we can't fail
        mHour = state.mHour;
        mWindSpeed = state.mWindSpeed;
        mCurrentWeather.swap(state.mCurrentWeather);
        mNextWeather.swap(state.mNextWeather);
        mCurrentRegion.swap(state.mCurrentRegion);
        mFirstUpdate = state.mFirstUpdate;
        mRemainingTransitionTime = state.mRemainingTransitionTime;
        mTimePassed = state.mTimePassed;

        return true;
    }

    return false;
}

void WeatherManager::clear()
{
    stopSounds();
    mRegionOverrides.clear();
    mRegionMods.clear();
    mThunderFlash = 0.0;
    mThunderChance = 0.0;
    mThunderChanceNeeded = 50.0;
}

void WeatherManager::switchToNextWeather(bool instantly)
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (world->isCellExterior() || world->isCellQuasiExterior())
    {
        std::string regionstr = Misc::StringUtils::lowerCase(world->getPlayerPtr().getCell()->getCell()->mRegion);

        if (mWeatherUpdateTime <= 0 || regionstr != mCurrentRegion)
        {
            mCurrentRegion = regionstr;
            mWeatherUpdateTime = mHoursBetweenWeatherChanges * 3600;

            std::string weatherType = "clear";

            if (mRegionOverrides.find(regionstr) != mRegionOverrides.end())
            {
                weatherType = mRegionOverrides[regionstr];
            }
            else
            {
                // get weather probabilities for the current region
                const ESM::Region *region = world->getStore().get<ESM::Region>().search (regionstr);

                if (region != 0)
                {
                    weatherType = nextWeather(region);
                }
            }

            setWeather(weatherType, instantly);
        }
    }
}

bool WeatherManager::isInStorm() const
{
    return mIsStorm;
}

osg::Vec3f WeatherManager::getStormDirection() const
{
    return mStormDirection;
}

void WeatherManager::advanceTime(double hours)
{
    mTimePassed += hours*3600;
}

inline void WeatherManager::addWeather(const std::string& name,
                                       const MWWorld::Fallback& fallback,
                                       const std::string& ambientLoopSoundID,
                                       const std::string& particleEffect)
{
    static const float fStromWindSpeed = mStore->get<ESM::GameSetting>().find("fStromWindSpeed")->getFloat();

    Weather weather(name, fallback, fStromWindSpeed, mRainSpeed, ambientLoopSoundID, particleEffect);

    std::string lower = name;
    lower[0] = tolower(lower[0]);
    mWeatherSettings.insert(std::make_pair(lower, weather));
}

inline Weather& WeatherManager::findWeather(const std::string& name)
{
    return mWeatherSettings.at(name);
}

