#pragma once

#include <cmath>
#include <algorithm>

class Envelope
{
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    struct Params
    {
        float attack = 0.01f;   // seconds
        float decay = 0.1f;
        float sustain = 0.7f;   // 0-1
        float release = 0.3f;

        // The shape of each stage, each on its own: -1 = log (most of the move comes early: an
        // attack that bulges upward, a decay or release that drops at once and then levels out),
        // 0 = a straight line, +1 = exp (most of the move comes late: a hollow attack, a decay or
        // release that holds and then falls).
        float attackCurve = 0.0f;
        float decayCurve = 0.0f;
        float releaseCurve = 0.0f;
    };

    // How far a stage has moved (0..1) when the fraction x (0..1) of its time has passed.
    static float shape(float x, float curve) { return applyCurve(x, curve); }

    // The inverse: the fraction of the stage's time at which it has moved by y.
    static float unshape(float y, float curve)
    {
        y = std::min(std::max(y, 0.0f), 1.0f);
        if (curve < -0.01f)
            return 1.0f - std::pow(1.0f - y, 1.0f / (1.0f - curve));
        if (curve > 0.01f)
            return std::pow(y, 1.0f / (1.0f + curve));
        return y;
    }

    void setSampleRate(double sr) { sampleRate = sr; dtPerSample = 1.0f / (float)sr; }
    void setParams(const Params& p)
    {
        // A curve changed under the stage that is running: carry on from the point of the new
        // curve that has the present height, so the level does not jump (no click).
        if (stage == Stage::Attack)       rebend(params.attackCurve, p.attackCurve);
        else if (stage == Stage::Decay)   rebend(params.decayCurve, p.decayCurve);
        else if (stage == Stage::Release) rebend(params.releaseCurve, p.releaseCurve);
        retarget(p.sustain, p.decayCurve);
        params = p;
        cachedAttackExp = computeExponent(p.attackCurve);
        cachedDecayExp = computeExponent(p.decayCurve);
        cachedReleaseExp = computeExponent(p.releaseCurve);
    }

    void setAttack(float v) { params.attack = v; }
    void setDecay(float v) { params.decay = v; }
    void setSustain(float v) { retarget(v, params.decayCurve); params.sustain = v; }
    void setRelease(float v) { params.release = v; }
    const Params& getParams() const { return params; }

    // Every stage time (attack, decay, release) times this, 1/64 .. 64; the params stay as set.
    // What it is for: the per-note spread of envelope times (EVAR) and envelope time as a
    // modulation destination (mod::DestTable::envTimes: ETIM, FTIM), with no second copy of the
    // params. Set under a sounding note it changes how fast the running stage goes on from where
    // it is: the level does not jump (a stage keeps its place as a fraction of its time). At 1 the
    // envelope is, bit for bit, what it is without it (x * 1.0f is x).
    static constexpr float MIN_TIME_SCALE = 1.0f / 64.0f, MAX_TIME_SCALE = 64.0f;
    void setTimeScale(float scale)
    {
        timeScale = scale >= MIN_TIME_SCALE ? (scale <= MAX_TIME_SCALE ? scale : MAX_TIME_SCALE)
                                            : (scale < MIN_TIME_SCALE ? MIN_TIME_SCALE : 1.0f);    // (not a number: none)
    }
    float getTimeScale() const { return timeScale; }

    void noteOn()
    {
        stage = Stage::Attack;
        stageProgress = 0.0f;
        startLevel = currentLevel;
    }

    void noteOff()
    {
        if (stage != Stage::Idle)
        {
            stage = Stage::Release;
            stageProgress = 0.0f;
            startLevel = currentLevel;
        }
    }

    void reset()
    {
        stage = Stage::Idle;
        currentLevel = 0.0f;
        stageProgress = 0.0f;
    }

    float process()
    {
        float dt = dtPerSample;

        switch (stage)
        {
            case Stage::Idle:
                currentLevel = 0.0f;
                break;

            case Stage::Attack:
            {
                float duration = std::max(params.attack * timeScale, 0.001f);
                stageProgress += dt / duration;
                if (stageProgress >= 1.0f)
                {
                    currentLevel = 1.0f;
                    stage = Stage::Decay;
                    stageProgress = 0.0f;
                    startLevel = 1.0f;
                    decayFrom = 1.0f;
                }
                else
                {
                    currentLevel = startLevel + (1.0f - startLevel) * applyCurveFast(stageProgress, params.attackCurve, cachedAttackExp);
                }
                break;
            }

            case Stage::Decay:
            {
                float duration = std::max(params.decay * timeScale, 0.001f);
                stageProgress += dt / duration;
                if (stageProgress >= 1.0f)
                {
                    currentLevel = params.sustain;
                    stage = Stage::Sustain;
                    stageProgress = 0.0f;
                }
                else
                {
                    currentLevel = decayFrom + (params.sustain - decayFrom) * applyCurveFast(stageProgress, params.decayCurve, cachedDecayExp);
                }
                break;
            }

            case Stage::Sustain:
                // A sustain level that is set under a held note (a step lock, an LFO with an
                // edge) is moved to over at most 4 ms, never jumped to: a jump is a click.
                if (currentLevel != params.sustain)
                {
                    const float d = params.sustain - currentLevel, step = dt * 250.0f;
                    currentLevel = std::abs(d) <= step ? params.sustain : currentLevel + (d > 0.0f ? step : -step);
                }
                break;

            case Stage::Release:
            {
                float duration = std::max(params.release * timeScale, 0.001f);
                stageProgress += dt / duration;
                if (stageProgress >= 1.0f)
                {
                    currentLevel = 0.0f;
                    stage = Stage::Idle;
                    stageProgress = 0.0f;
                }
                else
                {
                    currentLevel = startLevel * (1.0f - applyCurveFast(stageProgress, params.releaseCurve, cachedReleaseExp));
                }
                break;
            }
        }

        return currentLevel;
    }

    float getCurrentLevel() const { return currentLevel; }
    Stage getStage() const { return stage; }
    bool isActive() const { return stage != Stage::Idle; }

private:
    // The sustain level changed while the decay is on its way to it: the decay goes on from the
    // present level towards the new one (its start is moved, so nothing jumps).
    void retarget(float sustain, float curve)
    {
        if (stage != Stage::Decay || sustain == params.sustain)
            return;
        const float c = applyCurve(stageProgress, curve);
        if (c < 0.999f)
            decayFrom = (currentLevel - sustain * c) / (1.0f - c);
    }

    void rebend(float oldCurve, float newCurve)
    {
        if (oldCurve != newCurve && stageProgress > 0.0f && stageProgress < 1.0f)
            stageProgress = std::min(std::max(unshape(applyCurve(stageProgress, oldCurve), newCurve), 0.0f), 1.0f);
    }

    static float computeExponent(float curve)
    {
        if (curve < -0.01f)
            return 1.0f - curve;
        if (curve > 0.01f)
            return 1.0f + curve;
        return 1.0f;
    }

    static float applyCurve(float x, float curve)
    {
        if (curve < -0.01f)
            return 1.0f - std::pow(1.0f - x, 1.0f - curve);
        if (curve > 0.01f)
            return std::pow(x, 1.0f + curve);
        return x;
    }

    static float applyCurveFast(float x, float curve, float cachedExp)
    {
        if (curve < -0.01f)
            return 1.0f - std::pow(1.0f - x, cachedExp);
        if (curve > 0.01f)
            return std::pow(x, cachedExp);
        return x;
    }

    double sampleRate = 44100.0;
    float dtPerSample = 1.0f / 44100.0f;
    Params params;
    float timeScale = 1.0f;
    Stage stage = Stage::Idle;
    float currentLevel = 0.0f;
    float stageProgress = 0.0f;
    float startLevel = 0.0f;
    float decayFrom = 1.0f;         // where the decay started (1, unless the sustain moved under it)
    float cachedAttackExp = 1.0f;
    float cachedDecayExp = 1.0f;
    float cachedReleaseExp = 1.0f;
};
