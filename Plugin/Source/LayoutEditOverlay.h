#pragma once
#include <JuceHeader.h>
#include <optional>
#include "LayoutTweaks.h"
#include "Theme.h"
#include "R3WRKLookAndFeel.h"

/**
    Edit Layout mode: sits over the knob row + FX drawer while LayoutTweaks::isEditing(), and
    swallows every mouse event there so knobs can't turn by accident. Each movable item gets a
    dashed outline; drag one sideways to nudge it (snaps to the edges / centres of the other
    items' knob discs and toggles, with a guide line -- hold Option to drag freely), click one
    then use the arrow keys for 1px nudges (Shift = 10px), double-click or Delete to put one
    back where the automatic layout wants it. [DONE] / [RESET] sit over the drawer icon;
    Escape or Return also finish.

    The rows own where things go (they apply the offsets in their own resized()); this only
    edits the numbers in LayoutTweaks and repaints when the editor re-lays out.
*/
class LayoutEditOverlay : public juce::Component
{
public:
    std::function<juce::Array<LayoutItem>()> getItems;   // rects in each item's owner's coords
    std::function<void()> onDone;

    LayoutEditOverlay()
    {
        setWantsKeyboardFocus(true);
        setMouseClickGrabsKeyboardFocus(true);
    }

    void paint(juce::Graphics& g) override
    {
        const auto& pal = theme->palette();
        g.fillAll(pal.windowBg.withAlpha(0.35f));

        const float dash[] = { 3.0f, 3.0f };
        for (const auto& it : localItems())
        {
            const bool sel = it.id == selectedId, hot = it.id == hoveredId;
            const auto r = it.bounds.toFloat().reduced(1.5f);
            g.setColour((sel ? pal.accent : hot ? pal.text : pal.textDim).withAlpha(sel || hot ? 0.9f : 0.55f));
            juce::Path p;
            p.addRectangle(r);
            juce::Path dashed;
            juce::PathStrokeType(1.0f).createDashedStroke(dashed, p, dash, 2);
            g.fillPath(dashed);

            const int dx = layout->get(it.id);
            if (sel || (hot && dx != 0))
            {
                g.setFont(systemUIFont(11.0f));
                g.drawText(dx == 0 ? juce::String("0") : (dx > 0 ? "+" : "") + juce::String(dx),
                           r.withHeight(13.0f).translated(0.0f, 2.0f), juce::Justification::centred);
            }
        }

        if (guideX)
        {
            g.setColour(pal.accent);
            g.fillRect((float) *guideX - 0.5f, 0.0f, 1.0f, (float) getHeight());
        }

        drawButton(g, doneArea(),  "DONE",  hoveredButton == 1);
        drawButton(g, resetArea(), "RESET", hoveredButton == 2);
    }

    void mouseMove(const juce::MouseEvent& e) override { updateHover(e.getPosition()); }
    void mouseExit(const juce::MouseEvent&) override { hoveredId = {}; hoveredButton = 0; repaint(); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragId = {};
        guideX.reset();
        if (doneArea().contains(e.getPosition()) || resetArea().contains(e.getPosition()))
            return;   // handled on mouseUp, like a button

        const auto items = localItems();
        const auto* it = itemAt(items, e.getPosition());
        selectedId = it != nullptr ? it->id : juce::String();
        if (it != nullptr)
        {
            dragId = it->id;
            dragStartOffset = layout->get(it->id);
            // Guides relative to the item's un-nudged position, plus everything it can snap to.
            ownGuides.clear();
            for (auto g : it->guides) ownGuides.add(g.translated(-dragStartOffset, 0));
            targetXs.clear();
            for (const auto& other : items)
            {
                if (other.id == it->id || dependsOn(other.id, it->id)) continue;
                for (auto g : other.guides)
                    targetXs.addArray({ g.getX(), g.getCentreX(), g.getRight() });
            }
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragId.isEmpty()) return;
        int dx = dragStartOffset + e.getDistanceFromDragStartX();
        guideX.reset();
        if (! e.mods.isAltDown())
        {
            int best = snapDistance + 1, bestX = 0;
            for (auto g : ownGuides)
                for (int x : { g.getX(), g.getCentreX(), g.getRight() })
                    for (int t : targetXs)
                        if (std::abs(t - (x + dx)) < std::abs(best))
                        {
                            best = t - (x + dx);
                            bestX = t;
                        }
            if (std::abs(best) <= snapDistance)
            {
                dx += best;
                guideX = bestX;
            }
        }
        layout->set(dragId, dx, false);   // the editor re-lays out on the change message
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragId.isNotEmpty())
        {
            layout->save();
            dragId = {};
            guideX.reset();
            repaint();
            return;
        }
        if (doneArea().contains(e.getPosition()))
        {
            if (onDone) onDone();
        }
        else if (resetArea().contains(e.getPosition()) && layout->hasAny())
        {
            juce::NativeMessageBox::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,
                "Reset Layout", "Put every knob and effect back in its automatic position?", this,
                juce::ModalCallbackFunction::create([ptr = juce::Component::SafePointer<LayoutEditOverlay>(this)](int ok)
                {
                    if (ok != 0 && ptr != nullptr) ptr->layout->resetAll();
                }));
        }
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        const auto items = localItems();
        if (const auto* it = itemAt(items, e.getPosition()))
            layout->set(it->id, 0);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey)
        {
            if (onDone) onDone();
            return true;
        }
        if (selectedId.isEmpty())
            return true;   // swallow everything else while editing (no Space-to-play mid-edit)
        const int step = key.getModifiers().isShiftDown() ? 10 : 1;
        if (key.getKeyCode() == juce::KeyPress::leftKey)  layout->set(selectedId, layout->get(selectedId) - step);
        if (key.getKeyCode() == juce::KeyPress::rightKey) layout->set(selectedId, layout->get(selectedId) + step);
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
            layout->set(selectedId, 0);
        repaint();
        return true;
    }

private:
    static constexpr int snapDistance = 4;

    // Drawer items lay out relative to knob-row items (the first slot follows Pitch's disc,
    // Gain follows End), so they'd move along with the drag -- don't snap to them.
    static bool dependsOn(const juce::String& other, const juce::String& dragged)
    {
        if (other.startsWith(dragged + ".")) return true;   // a slot's own toggle moves with it
        if (dragged == "knob.Pitch") return other.startsWith("fx.");
        if (dragged == "knob.End")   return other == "fx.gain";
        return false;
    }

    juce::Array<LayoutItem> localItems() const
    {
        juce::Array<LayoutItem> out;
        if (! getItems) return out;
        for (auto it : getItems())
        {
            if (it.owner == nullptr) continue;
            it.bounds = getLocalArea(it.owner, it.bounds);
            for (auto& g : it.guides) g = getLocalArea(it.owner, g);
            out.add(it);
        }
        return out;
    }

    static const LayoutItem* itemAt(const juce::Array<LayoutItem>& items, juce::Point<int> p)
    {
        for (int i = items.size(); --i >= 0;)
            if (items.getReference(i).bounds.contains(p))
                return &items.getReference(i);
        return nullptr;
    }

    void updateHover(juce::Point<int> p)
    {
        const int b = doneArea().contains(p) ? 1 : resetArea().contains(p) ? 2 : 0;
        const auto items = localItems();
        const auto* it = b == 0 ? itemAt(items, p) : nullptr;
        const auto id = it != nullptr ? it->id : juce::String();
        if (id != hoveredId || b != hoveredButton)
        {
            hoveredId = id;
            hoveredButton = b;
            setMouseCursor(it != nullptr ? juce::MouseCursor::LeftRightResizeCursor
                                         : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    // Over the knob row's drawer icon (its right-hand 54px, top 83px strip).
    juce::Rectangle<int> buttonColumn() const { return getLocalBounds().removeFromTop(83).removeFromRight(54); }
    juce::Rectangle<int> doneArea()  const { auto c = buttonColumn(); return c.withSizeKeepingCentre(c.getWidth(), 20).translated(0, -12); }
    juce::Rectangle<int> resetArea() const { auto c = buttonColumn(); return c.withSizeKeepingCentre(c.getWidth(), 20).translated(0, 12); }

    void drawButton(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, bool hot) const
    {
        const auto& pal = theme->palette();
        const bool enabled = text != "RESET" || layout->hasAny();
        g.setColour(! enabled ? pal.textDim.withAlpha(0.4f) : hot ? pal.text : pal.textDim);
        g.setFont(systemUIFont(12.0f, hot && enabled));
        g.drawFittedText("[" + text + "]", r, juce::Justification::centred, 1, 0.8f);
    }

    juce::SharedResourcePointer<LayoutTweaks> layout;
    juce::SharedResourcePointer<ThemeManager> theme;

    juce::String hoveredId, selectedId, dragId;
    int hoveredButton = 0;
    int dragStartOffset = 0;
    juce::Array<juce::Rectangle<int>> ownGuides;
    juce::Array<int> targetXs;
    std::optional<int> guideX;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LayoutEditOverlay)
};
