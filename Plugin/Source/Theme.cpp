#include "Theme.h"

//==============================================================================
const PaletteField kPaletteFields[] =
{
    { "windowBg",     "window background", &Palette::windowBg },
    { "panelBg",      "panel background",  &Palette::panelBg },
    { "waveform",     "waveform",          &Palette::waveform },
    { "accent",       "accent",            &Palette::accent },
    { "zeroLine",     "zero line",         &Palette::zeroLine },
    { "gridLine",     "grid / lane lines", &Palette::gridLine },
    { "playhead",     "playhead",          &Palette::playhead },
    { "loopMarker",   "loop / unsaved",    &Palette::loopMarker },
    { "recordButton", "record button",     &Palette::recordButton },
    { "text",         "text",              &Palette::text },
    { "textDim",      "dim text",          &Palette::textDim },
};
const int kNumPaletteFields = (int) (sizeof(kPaletteFields) / sizeof(kPaletteFields[0]));

//==============================================================================
juce::String Palette::toString() const
{
    juce::StringArray parts;
    for (int i = 0; i < kNumPaletteFields; ++i)
    {
        const auto& f = kPaletteFields[i];
        parts.add(juce::String(f.key) + ":" + (this->*(f.member)).toDisplayString(true));
    }
    return parts.joinIntoString(";");
}

Palette Palette::fromString(const juce::String& s)
{
    Palette p;   // defaults = Midnight
    for (auto& tok : juce::StringArray::fromTokens(s, ";", {}))
    {
        auto key = tok.upToFirstOccurrenceOf(":", false, false).trim();
        auto val = tok.fromFirstOccurrenceOf(":", false, false).trim();
        if (key.isEmpty() || val.isEmpty())
            continue;
        for (int i = 0; i < kNumPaletteFields; ++i)
            if (key == kPaletteFields[i].key)
                p.*(kPaletteFields[i].member) = juce::Colour::fromString(val);
    }
    return p;
}

bool Palette::operator== (const Palette& o) const
{
    for (int i = 0; i < kNumPaletteFields; ++i)
        if ((this->*(kPaletteFields[i].member)) != (o.*(kPaletteFields[i].member)))
            return false;
    return true;
}

//==============================================================================
namespace
{
    struct BuiltIn { const char* name; const char* spec; };

    const BuiltIn kBuiltIns[] =
    {
        { "Midnight",
          "windowBg:ff14161a;panelBg:ff17191e;waveform:ff5ec2ff;accent:ff5ec2ff;"
          "zeroLine:29ffffff;gridLine:80000000;playhead:ffff3b30;loopMarker:ffffa500;"
          "recordButton:ff8b0000;text:ffe0e0e0;textDim:ff888888" },

        { "Slate",
          "windowBg:ff2a2e35;panelBg:ff232830;waveform:ff9db8d0;accent:ff8aa9c8;"
          "zeroLine:22ffffff;gridLine:66000000;playhead:ffff5b52;loopMarker:ffe6a552;"
          "recordButton:ff9e4444;text:ffdfe4ea;textDim:ff9aa3ad" },

        { "Graphite",
          "windowBg:ff1b1b1d;panelBg:ff202022;waveform:ffbfc2c8;accent:ff9a9aa2;"
          "zeroLine:20ffffff;gridLine:70000000;playhead:ffff453a;loopMarker:ffd8a53a;"
          "recordButton:ff8a3a3a;text:ffe6e6e8;textDim:ff8c8c92" },

        { "Amber",
          "windowBg:ff15120d;panelBg:ff1b1712;waveform:ffe0a35a;accent:ffffb454;"
          "zeroLine:22ffffff;gridLine:66000000;playhead:ffff6a4d;loopMarker:ffffd24d;"
          "recordButton:ff8a3d1f;text:ffece2d2;textDim:ff9a8f7d" },

        { "Paper",
          "windowBg:fff4f2ec;panelBg:ffe9e6de;waveform:ff2f6ea5;accent:ff2f6ea5;"
          "zeroLine:18000000;gridLine:28000000;playhead:ffd0402c;loopMarker:ffc07f18;"
          "recordButton:ffb23b3b;text:ff1f242b;textDim:ff6f7680" },
    };
    const int kNumBuiltIns = (int) (sizeof(kBuiltIns) / sizeof(kBuiltIns[0]));

    const juce::String kCustomPrefix = "custom.";
    const juce::String kActiveKey    = "activePalette";
}

//==============================================================================
ThemeManager::ThemeManager()
{
    load();
}

ThemeManager::~ThemeManager()
{
    stopTimer();
    if (propsFile != nullptr)
        propsFile->saveIfNeeded();
}

juce::PropertiesFile& ThemeManager::props()
{
    if (propsFile == nullptr)
    {
        juce::PropertiesFile::Options o;
        o.applicationName     = "R3WRK";
        o.filenameSuffix      = "settings";
        o.folderName          = "R3WRK";
        o.osxLibrarySubFolder = "Application Support";
        propsFile = std::make_unique<juce::PropertiesFile>(o);
    }
    return *propsFile;
}

void ThemeManager::load()
{
    const auto stored = props().getValue(kActiveKey, {});
    active = stored.isNotEmpty() ? Palette::fromString(stored) : getPreset("Midnight");
}

void ThemeManager::setPalette(const Palette& p)
{
    active = p;
    props().setValue(kActiveKey, p.toString());
    sendChangeMessage();
    startTimer(600);   // debounce the file write
}

void ThemeManager::timerCallback()
{
    stopTimer();
    if (propsFile != nullptr)
        propsFile->saveIfNeeded();
}

juce::StringArray ThemeManager::builtInNames() const
{
    juce::StringArray names;
    for (int i = 0; i < kNumBuiltIns; ++i)
        names.add(kBuiltIns[i].name);
    return names;
}

juce::StringArray ThemeManager::customNames() const
{
    juce::StringArray names;
    if (propsFile != nullptr)
        for (auto& key : propsFile->getAllProperties().getAllKeys())
            if (key.startsWith(kCustomPrefix))
                names.add(key.substring(kCustomPrefix.length()));
    names.sort(true);
    return names;
}

bool ThemeManager::isCustom(const juce::String& name) const
{
    return customNames().contains(name);
}

Palette ThemeManager::getPreset(const juce::String& name) const
{
    for (int i = 0; i < kNumBuiltIns; ++i)
        if (name == kBuiltIns[i].name)
            return Palette::fromString(kBuiltIns[i].spec);

    if (propsFile != nullptr)
    {
        const auto v = propsFile->getValue(kCustomPrefix + name, {});
        if (v.isNotEmpty())
            return Palette::fromString(v);
    }
    return Palette::fromString(kBuiltIns[0].spec);   // Midnight
}

void ThemeManager::saveCustom(const juce::String& name, const Palette& p)
{
    const auto clean = name.trim();
    if (clean.isEmpty())
        return;
    props().setValue(kCustomPrefix + clean, p.toString());
    props().saveIfNeeded();
    sendChangeMessage();
}

void ThemeManager::deleteCustom(const juce::String& name)
{
    props().removeValue(kCustomPrefix + name.trim());
    props().saveIfNeeded();
    sendChangeMessage();
}
