#pragma once
#include <JuceHeader.h>

/**
    A CallOutBox popup driven by a button, that can be pinned open.

      - single click on the button: opens the popup; any click outside it (other than on the
        button) closes it again, like an ordinary CallOutBox.
      - double-click on the button: pins it -- it stays up while the rest of the UI is used,
        until the button is clicked again.

    A plain CallOutBox::launchAsynchronously() popup can't do this: it runs modally, and a
    modal CallOutBox swallows the second click of a double-click (on the button that opened
    it) to dismiss itself. So the box here is non-modal, a child of the button's top-level
    component, with the "click outside closes it" part done by a global mouse listener that
    stands down once pinned.

    The effect panels (Mimeophon/Plexiphon/Reverb) each own one, for their "more" button.
*/
class PinnableCallout : private juce::MouseListener,
                        private juce::Timer
{
public:
    using ContentFactory = std::function<std::unique_ptr<juce::Component>()>;

    ~PinnableCallout() override { close(); }

    // Hook this up as the button's onClick.
    void buttonClicked(juce::Component& buttonIn, const ContentFactory& makeContent)
    {
        const auto now = juce::Time::getMillisecondCounter();

        if (box == nullptr)
        {
            open(buttonIn, makeContent);
            openedAt = now;
        }
        else if (! pinned && now - openedAt <= (juce::uint32) juce::MouseEvent::getDoubleClickTimeout())
        {
            pinned = true;   // second click of a double-click
        }
        else
        {
            close();
        }
    }

    void close()
    {
        stopTimer();
        juce::Desktop::getInstance().removeGlobalMouseListener(this);
        box.reset();
        content.reset();
        button = nullptr;
        pinned = false;
    }

private:
    void open(juce::Component& buttonIn, const ContentFactory& makeContent)
    {
        auto* parent = buttonIn.getTopLevelComponent();
        if (parent == nullptr)
            return;

        button = &buttonIn;
        content = makeContent();
        lastArea = targetArea();
        box = std::make_unique<juce::CallOutBox>(*content, lastArea, parent);   // adds itself to parent
        box->toFront(true);

        juce::Desktop::getInstance().addGlobalMouseListener(this);
        startTimerHz(10);
    }

    juce::Rectangle<int> targetArea() const
    {
        auto* parent = button->getTopLevelComponent();
        return parent->getLocalArea(button.getComponent(), button->getLocalBounds());
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (pinned || box == nullptr)
            return;
        auto* c = e.eventComponent;
        if (c == box.get() || box->isParentOf(c) || c == button.getComponent())
            return;   // the button's own click is handled by buttonClicked()
        juce::MessageManager::callAsync([safe = juce::WeakReference<PinnableCallout>(this)]
                                        { if (safe != nullptr) safe->close(); });
    }

    // Follow the button if the window resizes/reflows (e.g. the FX drawer toggling moves it),
    // and close if the button goes away (drawer closed, panel hidden).
    void timerCallback() override
    {
        if (button == nullptr || ! button->isShowing() || ! box->isVisible())   // Escape hides the box
        {
            close();
            return;
        }
        const auto area = targetArea();
        if (area != lastArea)
        {
            lastArea = area;
            box->updatePosition(area, button->getTopLevelComponent()->getLocalBounds());
        }
    }

    std::unique_ptr<juce::Component> content;
    std::unique_ptr<juce::CallOutBox> box;   // declared after content: destroyed first
    juce::Component::SafePointer<juce::Component> button;
    juce::Rectangle<int> lastArea;
    juce::uint32 openedAt = 0;
    bool pinned = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE(PinnableCallout)
};
