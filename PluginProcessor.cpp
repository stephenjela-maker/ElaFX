#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <algorithm>

class ELAFXAudioProcessor : public juce::AudioProcessor {
public:
    ELAFXAudioProcessor()
        : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          apvts(*this, nullptr, "Parameters", createParameterLayout()) {}

    ~ELAFXAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override {
        currentSampleRate = sampleRate;
        delayBuffer.setSize(2, DELAY_BUFFER_SIZE);
        delayBuffer.clear();
        sliceBuffer.setSize(2, SLICE_BUFFER_SIZE);
        sliceBuffer.clear();

        juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32>(samplesPerBlock), 1 };
        filterL.prepare(spec);
        filterR.prepare(spec);
        filterL.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
        filterR.setType(juce::dsp::StateVariableTPTFilterType::lowpass);

        juce::dsp::ProcessSpec revSpec { sampleRate, static_cast<juce::uint32>(samplesPerBlock), 2 };
        reverb.prepare(revSpec);
    }

    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        const int totalChannels = getTotalNumOutputChannels();
        const int numSamples = buffer.getNumSamples();

        if (auto* playHead = getPlayHead()) {
            if (auto pos = playHead->getPosition()) {
                if (pos->getBpm()) currentBpm = *pos->getBpm();
            }
        }

        const float spb = static_cast<float>((60.0 / std::max(40.0, currentBpm)) * currentSampleRate);

        const int timeIdx = static_cast<int>(*apvts.getRawParameterValue("Time"));
        float beatMul = 0.5f;
        if (timeIdx == 0) beatMul = 0.25f;
        else if (timeIdx == 1) beatMul = 0.5f;
        else if (timeIdx == 2) beatMul = 0.75f;
        else if (timeIdx == 3) beatMul = 1.0f;
        else if (timeIdx == 4) beatMul = 2.0f;

        const int delaySamples = std::clamp(static_cast<int>(spb * beatMul), 32, DELAY_BUFFER_SIZE - 2000);

        const int glitchRateIdx = static_cast<int>(*apvts.getRawParameterValue("GlitchRate"));
        float gMul = 0.25f;
        if (glitchRateIdx == 0) gMul = 0.125f;
        else if (glitchRateIdx == 1) gMul = 0.25f;
        else if (glitchRateIdx == 2) gMul = 0.5f;
        else if (glitchRateIdx == 3) gMul = 1.0f;
        const int baseSlice = std::max(64, static_cast<int>(spb * gMul));

        const float feedback = *apvts.getRawParameterValue("Feedback") * 0.01f;
        const float glitchProb = *apvts.getRawParameterValue("GlitchProb") * 0.01f;
        const int stutterMode = static_cast<int>(*apvts.getRawParameterValue("StutterMode"));
        const int crushBits = static_cast<int>(*apvts.getRawParameterValue("Crush"));
        const float crushSteps = std::pow(2.0f, static_cast<float>(crushBits));
        const float driveGain = std::pow(10.0f, *apvts.getRawParameterValue("Drive") / 20.0f);
        const float baseCutoff = *apvts.getRawParameterValue("FilterCutoff");
        const int sweepRateIdx = static_cast<int>(*apvts.getRawParameterValue("SweepRate"));
        const float sweepDepth = *apvts.getRawParameterValue("SweepDepth") * 0.01f;
        const float panNorm = (*apvts.getRawParameterValue("Pan") + 100.0f) * 0.005f;
        const float pingPong = *apvts.getRawParameterValue("PingPong") * 0.01f;
        const float mix = *apvts.getRawParameterValue("Mix") * 0.01f;
        const float revMix = *apvts.getRawParameterValue("ReverbMix") * 0.01f;

        juce::Reverb::Parameters rParams;
        rParams.roomSize = *apvts.getRawParameterValue("ReverbSize") * 0.01f;
        rParams.damping = 0.5f;
        rParams.wetLevel = revMix;
        rParams.dryLevel = 1.0f - revMix;
        reverb.setParameters(rParams);

        auto* channelDataL = buffer.getWritePointer(0);
        auto* channelDataR = totalChannels > 1 ? buffer.getWritePointer(1) : channelDataL;
        auto* delayDataL = delayBuffer.getWritePointer(0);
        auto* delayDataR = delayBuffer.getWritePointer(1);
        auto* sliceDataL = sliceBuffer.getWritePointer(0);
        auto* sliceDataR = sliceBuffer.getWritePointer(1);

        for (int i = 0; i < numSamples; ++i) {
            beatCounter += 1.0;
            const float dryL = channelDataL[i];
            const float dryR = channelDataR[i];

            // Glitch stutter engine
            if (triggerCountdown <= 0) {
                triggerCountdown = baseSlice;
                if (glitchProb > 0.001f && (static_cast<float>(std::rand()) / RAND_MAX) < glitchProb) {
                    isGlitching = true;
                    sliceLength = std::min(SLICE_BUFFER_SIZE - 1, baseSlice);
                    const int rep = 1 + (std::rand() % 3);
                    glitchRemaining = sliceLength * rep;
                    slicePlayPos = 0.0f;
                    activeGlitchMode = (stutterMode == 3) ? (std::rand() % 3) : stutterMode;

                    for (int s = 0; s < sliceLength; ++s) {
                        const int cIdx = (writePosition - sliceLength + s + DELAY_BUFFER_SIZE) % DELAY_BUFFER_SIZE;
                        sliceDataL[s] = delayDataL[cIdx];
                        sliceDataR[s] = delayDataR[cIdx];
                    }
                } else {
                    isGlitching = false;
                    glitchRemaining = 0;
                }
            } else {
                --triggerCountdown;
            }

            const int rIdx = (writePosition - delaySamples + DELAY_BUFFER_SIZE) % DELAY_BUFFER_SIZE;
            float wetL = delayDataL[rIdx];
            float wetR = delayDataR[rIdx];

            if (isGlitching && glitchRemaining > 0 && sliceLength > 32) {
                --glitchRemaining;
                const float rPos = (activeGlitchMode == 1) ? (static_cast<float>(sliceLength - 1) - slicePlayPos) : slicePlayPos;
                const int idx0 = static_cast<int>(rPos) % sliceLength;
                const int idx1 = (idx0 + 1) % sliceLength;
                const float fr = rPos - static_cast<int>(rPos);

                wetL = sliceDataL[idx0] * (1.0f - fr) + sliceDataL[idx1] * fr;
                wetR = sliceDataR[idx0] * (1.0f - fr) + sliceDataR[idx1] * fr;

                slicePlayPos += (activeGlitchMode == 2) ? 2.0f : 1.0f;
                if (slicePlayPos >= static_cast<float>(sliceLength)) slicePlayPos -= static_cast<float>(sliceLength);
                if (glitchRemaining <= 0) isGlitching = false;
            }

            // Bitcrush
            if (crushBits < 16) {
                wetL = std::round(wetL * crushSteps) / crushSteps;
                wetR = std::round(wetR * crushSteps) / crushSteps;
            }

            // Drive / Distortion
            if (driveGain > 1.01f) {
                wetL = std::tanh(wetL * driveGain);
                wetR = std::tanh(wetR * driveGain);
            }

            // Filter Sweep LFO
            float effCutoff = baseCutoff;
            if (sweepRateIdx > 0 && sweepDepth > 0.001f) {
                float bpc = 4.0f;
                if (sweepRateIdx == 1) bpc = 16.0f;
                else if (sweepRateIdx == 2) bpc = 8.0f;
                else if (sweepRateIdx == 3) bpc = 4.0f;
                else if (sweepRateIdx == 4) bpc = 2.0f;
                else if (sweepRateIdx == 5) bpc = 1.0f;
                else if (sweepRateIdx == 6) bpc = 0.5f;

                const float phase = std::fmod(static_cast<float>(beatCounter) / (bpc * spb), 1.0f);
                const float lfo = std::sin(phase * 6.2831853f);
                effCutoff = std::clamp(baseCutoff * std::pow(2.0f, lfo * sweepDepth * 2.5f), 100.0f, 18000.0f);
            }

            filterL.setCutoffFrequency(effCutoff);
            filterR.setCutoffFrequency(effCutoff);
            const float filteredL = filterL.processSample(0, wetL);
            const float filteredR = filterR.processSample(0, wetR);

            // Feedback & Ping Pong
            const float fbL = dryL + (filteredL * (1.0f - pingPong) + filteredR * pingPong) * feedback;
            const float fbR = dryR + (filteredR * (1.0f - pingPong) + filteredL * pingPong) * feedback;
            delayDataL[writePosition] = std::clamp(fbL, -1.5f, 1.5f);
            delayDataR[writePosition] = std::clamp(fbR, -1.5f, 1.5f);
            writePosition = (writePosition + 1) % DELAY_BUFFER_SIZE;

            // Pan
            const float panL = std::cos(panNorm * 1.5707963f);
            const float panR = std::sin(panNorm * 1.5707963f);
            channelDataL[i] = dryL * (1.0f - mix) + (filteredL * panL) * mix;
            channelDataR[i] = dryR * (1.0f - mix) + (filteredR * panR) * mix;
        }

        juce::dsp::AudioBlock<float> block(buffer);
        reverb.process(juce::dsp::ProcessContextReplacing<float>(block));
    }

    juce::AudioProcessorEditor* createEditor() override {
        return new juce::GenericAudioProcessorEditor(*this);
    }
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ELA FX"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override {
        auto state = apvts.copyState();
        std::unique_ptr<juce::XmlElement> xml(state.createXml());
        copyXmlToBinary(*xml, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override {
        std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
        if (xmlState != nullptr && xmlState->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
    }

    juce::AudioProcessorValueTreeState apvts;

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout() {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
        params.push_back(std::make_unique<juce::AudioParameterChoice>("Time", "Time", juce::StringArray{"1/16", "1/8", "1/8D", "1/4", "1/2"}, 1));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("Feedback", "Feedback", 0.0f, 95.0f, 45.0f));
        params.push_back(std::make_unique<juce::AudioParameterChoice>("GlitchRate", "Glitch Rate", juce::StringArray{"1/32", "1/16", "1/8", "1/4"}, 1));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("GlitchProb", "Glitch Prob", 0.0f, 100.0f, 50.0f));
        params.push_back(std::make_unique<juce::AudioParameterChoice>("StutterMode", "Stutter Mode", juce::StringArray{"Repeat", "Reverse", "OctaveUp", "Random"}, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>("Crush", "Crush", 2, 16, 16));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("Drive", "Drive", 0.0f, 24.0f, 0.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("FilterCutoff", "Cutoff", 200.0f, 18000.0f, 6500.0f));
        params.push_back(std::make_unique<juce::AudioParameterChoice>("SweepRate", "Sweep Rate", juce::StringArray{"Off", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8"}, 3));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("SweepDepth", "Sweep Depth", 0.0f, 100.0f, 50.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("Pan", "Pan", -100.0f, 100.0f, 0.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("PingPong", "Ping-Pong", 0.0f, 100.0f, 0.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("ReverbSize", "Rev Size", 0.0f, 100.0f, 50.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("ReverbMix", "Rev Mix", 0.0f, 100.0f, 30.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("Mix", "Dry/Wet", 0.0f, 100.0f, 50.0f));
        return { params.begin(), params.end() };
    }

    static constexpr int DELAY_BUFFER_SIZE = 262144;
    juce::AudioBuffer<float> delayBuffer;
    int writePosition { 0 };

    static constexpr int SLICE_BUFFER_SIZE = 65536;
    juce::AudioBuffer<float> sliceBuffer;
    bool isGlitching { false };
    int sliceLength { 4410 };
    int glitchRemaining { 0 };
    float slicePlayPos { 0.0f };
    int activeGlitchMode { 0 };
    int triggerCountdown { 0 };

    juce::dsp::StateVariableTPTFilter<float> filterL, filterR;
    juce::dsp::Reverb reverb;

    double currentSampleRate { 44100.0 };
    double currentBpm { 120.0 };
    double beatCounter { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ELAFXAudioProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new ELAFXAudioProcessor();
}
