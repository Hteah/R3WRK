#pragma once
#include <JuceHeader.h>
#include <map>

/**
    User layout tweaks: a small horizontal nudge (px) per movable item in the knob row and the
    FX drawer, set by dragging things around in Edit Layout mode (LayoutEditOverlay). The rows
    still do their normal automatic layout first; each item is then shifted by its offset, so
    an untouched item (offset 0) sits exactly where it always did and a reset is just "clear".

    Process-wide (juce::SharedResourcePointer, like ThemeManager) and saved to its own file,
    ~/Library/Application Support/R3WRK/Layout.settings -- one layout for every window and
    plugin instance. Broadcasts a change whenever an offset or the editing flag changes; the
    editor listens and re-runs its layout.

    Ids: "knob.<caption>" (e.g. "knob.END"), "badge.filter", and "fx.mod" / "fx.dly" /
    "fx.space" / "fx.gain" for the drawer's slots, "fx.<slot>.toggle" for their toggles.
*/
class LayoutTweaks : public juce::ChangeBroadcaster
{
public:
    LayoutTweaks() { load(); }
    ~LayoutTweaks() override { save(); }

    int get(const juce::String& id) const
    {
        const auto it = offsets.find(id);
        return it != offsets.end() ? it->second : 0;
    }

    // `persist` false while a drag is in progress (called per mouse move); the drag's
    // mouseUp calls save() once.
    void set(const juce::String& id, int dx, bool persist = true)
    {
        dx = juce::jlimit(-400, 400, dx);
        if (get(id) == dx) return;
        if (dx == 0) offsets.erase(id); else offsets[id] = dx;
        if (persist) save();
        sendChangeMessage();
    }

    bool hasAny() const { return ! offsets.empty(); }

    void resetAll()
    {
        if (offsets.empty()) return;
        offsets.clear();
        save();
        sendChangeMessage();
    }

    bool isEditing() const { return editing; }
    void setEditing(bool on)
    {
        if (editing == on) return;
        editing = on;
        sendChangeMessage();
    }

    void save()
    {
        juce::StringArray parts;
        for (const auto& [id, dx] : offsets)
            parts.add(id + "=" + juce::String(dx));
        props().setValue("offsets", parts.joinIntoString(";"));
        props().saveIfNeeded();
    }

private:
    void load()
    {
        offsets.clear();
        for (const auto& part : juce::StringArray::fromTokens(props().getValue("offsets"), ";", ""))
        {
            const auto id = part.upToFirstOccurrenceOf("=", false, false).trim();
            const int dx = part.fromFirstOccurrenceOf("=", false, false).getIntValue();
            if (id.isNotEmpty() && dx != 0)
                offsets[id] = juce::jlimit(-400, 400, dx);
        }
    }

    juce::PropertiesFile& props()
    {
        if (propsFile == nullptr)
        {
            juce::PropertiesFile::Options o;
            o.applicationName     = "Layout";
            o.filenameSuffix      = "settings";
            o.folderName          = "R3WRK";
            o.osxLibrarySubFolder = "Application Support";
            propsFile = std::make_unique<juce::PropertiesFile>(o);
        }
        return *propsFile;
    }

    std::map<juce::String, int> offsets;
    bool editing = false;
    std::unique_ptr<juce::PropertiesFile> propsFile;
};

/** One movable thing, as a row reports it to the Edit Layout overlay. */
struct LayoutItem
{
    juce::String id;
    juce::Component* owner = nullptr;           // the row whose coordinates the rects are in
    juce::Rectangle<int> bounds;                // the whole item (outline + hit area)
    juce::Array<juce::Rectangle<int>> guides;   // what snaps: knob discs, toggles
};

/** The rect a component "reads as" for snapping: a rotary slider's drawn disc (mirrors
    R3WRKLookAndFeel::drawRotarySlider -- a circle inset 3px, centred in the rotary area),
    otherwise its bounds. In `c`'s parent's coordinates. */
inline juce::Rectangle<int> layoutGuideRect(juce::Component& c)
{
    if (auto* s = dynamic_cast<juce::Slider*>(&c); s != nullptr && s->isRotary())
    {
        const auto rotary = s->getLookAndFeel().getSliderLayout(*s).sliderBounds.toFloat().reduced(3.0f);
        const float d = juce::jmin(rotary.getWidth(), rotary.getHeight());
        return juce::Rectangle<float>(d, d).withCentre(rotary.getCentre()).toNearestInt() + s->getPosition();
    }
    return c.getBounds();
}
