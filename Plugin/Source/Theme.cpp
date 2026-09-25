#include "Theme.h"
#include <cmath>

//==============================================================================
const PaletteField kPaletteFields[] =
{
    { "windowBg",     "window background", &Palette::windowBg },
    { "panelBg",      "panel background",  &Palette::panelBg },
    { "popupBg",      "popup background",  &Palette::popupBg },
    { "popupInk",     "popup ink",         &Palette::popupInk },
    { "waveform",     "waveform",          &Palette::waveform },
    { "accent",       "accent",            &Palette::accent },
    { "zeroLine",     "zero line",         &Palette::zeroLine },
    { "gridLine",     "grid / lane lines", &Palette::gridLine },
    { "playhead",     "playhead",          &Palette::playhead },
    { "loopMarker",   "loop / unsaved",    &Palette::loopMarker },
    { "recordButton", "record button",     &Palette::recordButton },
    { "text",         "text",              &Palette::text },
    { "textDim",      "dim text",          &Palette::textDim },
    { "screenText",    "screen text",       &Palette::screenText },
    { "screenTextDim", "screen dim text",   &Palette::screenTextDim },
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
    if (shadedPanel)
        parts.add("shadedPanel:1");
    parts.add("edgeShadeDarken:" + juce::String(edgeShadeDarken, 3));
    parts.add("edgeShadeAlpha:" + juce::String(edgeShadeAlpha, 3));
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
        if (key == "shadedPanel")
        {
            p.shadedPanel = (val == "1");
            continue;
        }
        if (key == "edgeShadeDarken")
        {
            p.edgeShadeDarken = val.getFloatValue();
            continue;
        }
        if (key == "edgeShadeAlpha")
        {
            p.edgeShadeAlpha = val.getFloatValue();
            continue;
        }
        for (int i = 0; i < kNumPaletteFields; ++i)
            if (key == kPaletteFields[i].key)
                p.*(kPaletteFields[i].member) = juce::Colour::fromString(val);
    }
    return p;
}

juce::String Palette::toClipboardText(const juce::String& name) const
{
    return "THEME name=" + name.removeCharacters(";") + ";" + toString();
}

bool Palette::fromClipboardText(const juce::String& text, Palette& out, juce::String* name)
{
    auto t = text.trim();
    if (t.startsWith("THEME"))
        t = t.substring(5).trimStart();

    bool anyKnown = false;
    juce::String foundName;
    juce::StringArray kept;
    for (auto& tok : juce::StringArray::fromTokens(t, ";", {}))
    {
        const auto trimmed = tok.trim();
        if (trimmed.startsWith("name="))
        {
            foundName = trimmed.substring(5).trim();
            continue;
        }
        const auto key = trimmed.upToFirstOccurrenceOf(":", false, false).trim();
        if (key == "shadedPanel" || key == "edgeShadeDarken" || key == "edgeShadeAlpha")
            anyKnown = true;
        for (int i = 0; i < kNumPaletteFields; ++i)
            if (key == kPaletteFields[i].key)
                anyKnown = true;
        kept.add(trimmed);
    }
    if (! anyKnown)
        return false;
    out = fromString(kept.joinIntoString(";"));
    if (name != nullptr)
        *name = foundName;
    return true;
}

bool Palette::operator== (const Palette& o) const
{
    for (int i = 0; i < kNumPaletteFields; ++i)
        if ((this->*(kPaletteFields[i].member)) != (o.*(kPaletteFields[i].member)))
            return false;
    return shadedPanel == o.shadedPanel
        && std::abs(edgeShadeDarken - o.edgeShadeDarken) < 0.001f
        && std::abs(edgeShadeAlpha - o.edgeShadeAlpha) < 0.001f;
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
          "recordButton:ff8b0000;text:ffe0e0e0;textDim:ff888888;"
          "screenText:ffe0e0e0;screenTextDim:ff888888" },

        { "Slate",
          "windowBg:ff2a2e35;panelBg:ff232830;waveform:ff9db8d0;accent:ff8aa9c8;"
          "zeroLine:22ffffff;gridLine:66000000;playhead:ffff5b52;loopMarker:ffe6a552;"
          "recordButton:ff9e4444;text:ffdfe4ea;textDim:ff9aa3ad;"
          "screenText:ffdfe4ea;screenTextDim:ff9aa3ad" },

        { "Graphite",
          "windowBg:ff1b1b1d;panelBg:ff202022;waveform:ffbfc2c8;accent:ff9a9aa2;"
          "zeroLine:20ffffff;gridLine:70000000;playhead:ffff453a;loopMarker:ffd8a53a;"
          "recordButton:ff8a3a3a;text:ffe6e6e8;textDim:ff8c8c92;"
          "screenText:ffe6e6e8;screenTextDim:ff8c8c92" },

        { "Amber",
          "windowBg:ff15120d;panelBg:ff1b1712;waveform:ffe0a35a;accent:ffffb454;"
          "zeroLine:22ffffff;gridLine:66000000;playhead:ffff6a4d;loopMarker:ffffd24d;"
          "recordButton:ff8a3d1f;text:ffece2d2;textDim:ff9a8f7d;"
          "screenText:ffece2d2;screenTextDim:ff9a8f7d" },

        { "Paper",
          "windowBg:fff4f2ec;panelBg:ffe9e6de;waveform:ff2f6ea5;accent:ff2f6ea5;"
          "zeroLine:18000000;gridLine:28000000;playhead:ffd0402c;loopMarker:ffc07f18;"
          "recordButton:ffb23b3b;text:ff1f242b;textDim:ff6f7680;"
          "screenText:ff1f242b;screenTextDim:ff6f7680" },

        // Steel-blue instrument panel + warm amber accent, dark control band, rounded edges
        // (LookAndFeel side of that is a separate pass) -- the waveform/ruler "screen" stays
        // Midnight-dark on purpose, so screenText/screenTextDim reuse Midnight's text values
        // while text/textDim (the light chrome) get their own dark-on-light pair.
        { "Madrona",
          "windowBg:ff8797ac;panelBg:ff17191e;waveform:ff5ec2ff;accent:ffd4a24a;"
          "zeroLine:29ffffff;gridLine:80000000;playhead:ffff3b30;loopMarker:ffffa500;"
          "recordButton:ffc1503a;text:ff1b2433;textDim:ff46536a;"
          "screenText:ffe0e0e0;screenTextDim:ff888888" },

        // Brushed-silver panel body (from the "Panel Body (Silver Grey)" mockup): a light
        // grey chrome with the shadedPanel gradient (see PluginEditor::paint) on, in place
        // of Madrona's flat light windowBg. Screen stays Midnight-dark for the same reason
        // Madrona's does -- see the screenText/screenTextDim comment on Palette.
        { "Silver",
          "windowBg:ffc9cbce;panelBg:ff17191e;waveform:ff5ec2ff;accent:ff2f6ea5;"
          "zeroLine:29ffffff;gridLine:80000000;playhead:ffff3b30;loopMarker:ffffa500;"
          "recordButton:ff8b0000;text:ff2b2c2e;textDim:ff6e6f72;"
          "screenText:ffe0e0e0;screenTextDim:ff888888;shadedPanel:1;"
          "edgeShadeDarken:0.85;edgeShadeAlpha:0.42" },

        // Sieve's own presets, in the shared format: Sieve's surface = panelBg, its bars =
        // windowBg, divider = gridLine, accent = accent + waveform. Everything else is Midnight's
        // (they're all dark surfaces, so Midnight's light text fits). Kept identical to
        // Sieve's ThemePalette.builtIns so both apps list the same starting points.
        { "Ableton Dark Blue-Grey",
          "windowBg:ff293238;panelBg:ff37474f;gridLine:ff2c3a42;accent:fff5b854;waveform:fff5b854" },
        { "Charcoal",
          "windowBg:ff2e2e2e;panelBg:ff434343;gridLine:ff363636;accent:fff5b854;waveform:fff5b854" },
        { "Neutral Grey",
          "windowBg:ff3a3a3a;panelBg:ff616161;gridLine:ff4e4e4e;accent:fff5b854;waveform:fff5b854" },
        { "Slate Blue",
          "windowBg:ff21252e;panelBg:ff2e3440;gridLine:ff262b36;accent:ff88c0d0;waveform:ff88c0d0" },
        { "Warm Graphite",
          "windowBg:ff262322;panelBg:ff3a3736;gridLine:ff302c2b;accent:ffe0a24e;waveform:ffe0a24e" },
        { "Soft Grey",
          "windowBg:ff66666a;panelBg:ff46464b;gridLine:ff363636;accent:ffe7eaec;waveform:ffe7eaec" },
    };
    const int kNumBuiltIns = (int) (sizeof(kBuiltIns) / sizeof(kBuiltIns[0]));

    const juce::String kCustomPrefix = "custom.";
    const juce::String kActiveKey    = "activePalette";
    const juce::String kMigratedKey  = "migratedSharedThemes";
    const juce::String kThemeExt     = ".theme";

    // A theme name as a file name: no path separators or colons (Finder shows ':' as '/').
    juce::String fileSafe(const juce::String& name)
    {
        return name.trim().replaceCharacters("/:", "--");
    }
}

juce::File ThemeManager::sharedThemesDir()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("Application Support").getChildFile("Shared Themes");
    dir.createDirectory();
    return dir;
}

juce::File ThemeManager::themeFile(const juce::String& name)
{
    return sharedThemesDir().getChildFile(fileSafe(name) + kThemeExt);
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
    migrateSettingsThemes();
    const auto stored = props().getValue(kActiveKey, {});
    active = stored.isNotEmpty() ? Palette::fromString(stored) : getPreset("Midnight");
}

void ThemeManager::migrateSettingsThemes()
{
    auto& pf = props();
    if (pf.getBoolValue(kMigratedKey, false))
        return;
    // Themes saved before the shared folder existed lived as "custom.<name>" settings entries.
    // Copy each out to a .theme file (never clobbering one that's already there); the old
    // entries stay in the settings file untouched, as a backup.
    for (auto& key : pf.getAllProperties().getAllKeys())
    {
        if (! key.startsWith(kCustomPrefix))
            continue;
        const auto f = themeFile(key.substring(kCustomPrefix.length()));
        if (! f.existsAsFile())
            f.replaceWithText(pf.getValue(key));
    }
    pf.setValue(kMigratedKey, true);
    pf.saveIfNeeded();
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
    for (auto& f : sharedThemesDir().findChildFiles(juce::File::findFiles, false, "*" + kThemeExt))
        names.add(f.getFileNameWithoutExtension());
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

    const auto f = themeFile(name);
    if (f.existsAsFile())
        return Palette::fromString(f.loadFileAsString());
    return Palette::fromString(kBuiltIns[0].spec);   // Midnight
}

void ThemeManager::saveCustom(const juce::String& name, const Palette& p)
{
    const auto clean = name.trim();
    if (clean.isEmpty())
        return;
    themeFile(clean).replaceWithText(p.toString());
    sendChangeMessage();
}

void ThemeManager::deleteCustom(const juce::String& name)
{
    themeFile(name).deleteFile();
    sendChangeMessage();
}

void ThemeManager::copyToClipboard(const juce::String& name) const
{
    juce::SystemClipboard::copyTextToClipboard(active.toClipboardText(name));
}

bool ThemeManager::pasteFromClipboard(juce::String* name)
{
    Palette p;
    if (! Palette::fromClipboardText(juce::SystemClipboard::getTextFromClipboard(), p, name))
        return false;
    setPalette(p);
    return true;
}
