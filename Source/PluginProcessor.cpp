/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
SimpleEQAudioProcessor::SimpleEQAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
{
}

SimpleEQAudioProcessor::~SimpleEQAudioProcessor()
{
}

//==============================================================================
const juce::String SimpleEQAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool SimpleEQAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool SimpleEQAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool SimpleEQAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double SimpleEQAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int SimpleEQAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int SimpleEQAudioProcessor::getCurrentProgram()
{
    return 0;
}

void SimpleEQAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String SimpleEQAudioProcessor::getProgramName (int index)
{
    return {};
}

void SimpleEQAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void SimpleEQAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;

    spec.maximumBlockSize = samplesPerBlock;
    spec.numChannels = 1;
    spec.sampleRate = sampleRate;

    leftChain.prepare(spec);
    rightChain.prepare(spec);

    updateFilters();
}

void SimpleEQAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool SimpleEQAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void SimpleEQAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // In case we have more outputs than inputs, this code clears any output
    // channels that didn't contain input data, (because these aren't
    // guaranteed to be empty - they may contain garbage).
    // This is here to avoid people getting screaming feedback
    // when they first compile a plugin, but obviously you don't need to keep
    // this code if your algorithm always overwrites all the output channels.
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    updateFilters();
    juce::dsp::AudioBlock<float> block(buffer);
    
    auto leftBLock = block.getSingleChannelBlock(0);
    auto rightBLock = block.getSingleChannelBlock(1);

    juce::dsp::ProcessContextReplacing<float> leftContext(leftBLock);
    juce::dsp::ProcessContextReplacing<float> rightContext(rightBLock);

    leftChain.process(leftContext);
    rightChain.process(rightContext);
}

//==============================================================================
bool SimpleEQAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* SimpleEQAudioProcessor::createEditor()
{
    return new SimpleEQAudioProcessorEditor (*this);
}

//==============================================================================
void SimpleEQAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.

    juce::MemoryOutputStream mos(destData, true);
    apvts.state.writeToStream(mos);
}

void SimpleEQAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.

    auto tree = juce::ValueTree::readFromData(data, sizeInBytes);
    if (tree.isValid()) {
        apvts.replaceState(tree);
        updateFilters();
    }
}

ChainSettings getChainSettings(juce::AudioProcessorValueTreeState& apvts) {
    ChainSettings settings;

    settings.lowCutFreq = apvts.getRawParameterValue("lowCutFreq")->load();
    settings.highCutFreq = apvts.getRawParameterValue("highCutFreq")->load();
    settings.peakFreq = apvts.getRawParameterValue("peakFreq")->load();
    settings.peakQuality = apvts.getRawParameterValue("peakQuality")->load();
    settings.peakGainDecibels = apvts.getRawParameterValue("peakGain")->load();
    settings.highCutSlope = static_cast<Slope>(apvts.getRawParameterValue("highCutSlope")->load());
    settings.lowCutSlope = static_cast<Slope>(apvts.getRawParameterValue("lowCutSlope")->load());

    return settings;
}

void SimpleEQAudioProcessor::updatePeakFilter(const ChainSettings& chainSettings) {

    auto peakCoefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter(getSampleRate(), chainSettings.peakFreq, chainSettings.peakQuality,
        juce::Decibels::decibelsToGain(chainSettings.peakGainDecibels));
        updateCoefficients(leftChain.get<ChainPositions::Peak>().coefficients, peakCoefficients);
        updateCoefficients(rightChain.get<ChainPositions::Peak>().coefficients, peakCoefficients);
}

void SimpleEQAudioProcessor::updateLowCutFilter(const ChainSettings& chainSettings) {

    auto cutCoefficients = juce::dsp::FilterDesign<float>::designIIRHighpassHighOrderButterworthMethod(chainSettings.lowCutFreq,
        getSampleRate(),
        2 * (chainSettings.lowCutSlope + 1));
    auto& leftLowCut = leftChain.get<ChainPositions::LowCut>();
    auto& rightLowCut = rightChain.get<ChainPositions::LowCut>();

    updateCutFilter(leftLowCut, cutCoefficients, chainSettings.lowCutSlope);
    updateCutFilter(rightLowCut, cutCoefficients, chainSettings.lowCutSlope);

}
void SimpleEQAudioProcessor::updateHighCutFilter(const ChainSettings& chainSettings) {

    auto HighcutCoefficients = juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod(chainSettings.highCutFreq,
        getSampleRate(),
        2 * (chainSettings.highCutSlope + 1));
    auto& leftHighCut = leftChain.get<ChainPositions::HighCut>();
    auto& rightHighCut = rightChain.get<ChainPositions::HighCut>();

    updateCutFilter(leftHighCut, HighcutCoefficients, chainSettings.highCutSlope);
    updateCutFilter(rightHighCut, HighcutCoefficients, chainSettings.highCutSlope);

}

void SimpleEQAudioProcessor::updateFilters() {
    auto chainSettings = getChainSettings(apvts);


    updateLowCutFilter(chainSettings);
    updatePeakFilter(chainSettings);
    updateHighCutFilter(chainSettings);
}

void SimpleEQAudioProcessor::updateCoefficients(Coefficients& old, const Coefficients& replacments) {
    *old = *replacments;
}

juce::AudioProcessorValueTreeState::ParameterLayout SimpleEQAudioProcessor::createParameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    //low cut parameter
    layout.add(std::make_unique<juce::AudioParameterFloat>("lowCutFreq", "LowCut Freq", juce::NormalisableRange<float>(20.f, 20000.f, 1.f, 0.4f), 20.f));
    //high cut parameter
    layout.add(std::make_unique<juce::AudioParameterFloat>("highCutFreq", "HighCut Freq", juce::NormalisableRange<float>(20.f, 20000.f, 1.f, 0.4f), 20000.f));
    //peak frequency
    layout.add(std::make_unique<juce::AudioParameterFloat>("peakFreq", "Peak Freq", juce::NormalisableRange<float>(20.f, 20000.f, 1.f, 0.4f), 750.f));
    //peak gain
    layout.add(std::make_unique<juce::AudioParameterFloat>("peakGain", "Peak Gain", juce::NormalisableRange<float>(-24.f, 24.f, 0.5f, 1.f),0.0f));
    //bell spread
    layout.add(std::make_unique<juce::AudioParameterFloat>("peakQuality", "Peak Quality", juce::NormalisableRange<float>(0.1f, 10.f, 0.05f, 1.f), 1.f));

    //slope choices
    juce::StringArray slopeChoices;
    for (int i = 0;i < 4;i++) {
        juce::String choice;
        choice << (12 + i * 12) << "db/Oct";
        slopeChoices.add(choice);
    }
    layout.add(std::make_unique<juce::AudioParameterChoice>("lowCutSlope", "LowCut Slope", slopeChoices, 0));
    layout.add(std::make_unique<juce::AudioParameterChoice>("highCutSlope", "HighCut Slope", slopeChoices, 0));

    return layout;
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SimpleEQAudioProcessor();
}
