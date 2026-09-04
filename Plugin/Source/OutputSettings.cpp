#include "OutputSettings.h"

namespace
{
    const juce::String kFolderKey = "outputFolder";

    juce::File defaultFolder()
    {
        return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("R3WRK");
    }
}

OutputSettings::OutputSettings() {}

OutputSettings::~OutputSettings()
{
    if (propsFile != nullptr)
        propsFile->saveIfNeeded();
}

juce::PropertiesFile& OutputSettings::props()
{
    if (propsFile == nullptr)
    {
        juce::PropertiesFile::Options o;
        o.applicationName     = "output";
        o.filenameSuffix      = "settings";
        o.folderName          = "R3WRK";
        o.osxLibrarySubFolder = "Application Support";
        propsFile = std::make_unique<juce::PropertiesFile>(o);
    }
    return *propsFile;
}

juce::File OutputSettings::folder()
{
    juce::File f (props().getValue(kFolderKey, {}));
    if (! f.isDirectory())
        f = defaultFolder();
    f.createDirectory();
    return f;
}

void OutputSettings::setFolder(const juce::File& dir)
{
    if (! dir.isDirectory())
        return;
    props().setValue(kFolderKey, dir.getFullPathName());
    props().saveIfNeeded();
}

juce::File OutputSettings::makeWavFile(bool isSelection)
{
    juce::String name = juce::Time::getCurrentTime().formatted("R3WRK %Y-%m-%d %H.%M.%S");
    if (isSelection)
        name << " selection";
    return folder().getChildFile(name + ".wav").getNonexistentSibling();
}
