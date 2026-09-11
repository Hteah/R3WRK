#pragma once
#include <JuceHeader.h>
#include "EditorToolbar.h"

/**
    The Standalone app's native macOS application menu bar: File / Edit / Tools / Slice,
    holding the same actions as the in-window "Tools ▾" button. Both surfaces build their
    menus from EditorToolbar::ToolsMenuItem ids and dispatch through
    EditorToolbar::performToolsItem(), so there's one source of truth for what each action does.

    The macOS application ("R3WRK") menu also gets three config items -- Audio Settings,
    Output Folder, Theme -- via getAppleMenuItems(), which PluginEditor hands to
    setMacMainMenu() as its extraAppleMenuItems. Those items are dropped from File / Tools /
    the in-window Tools ▾ when running Standalone so they live in just one place. Selections
    on them arrive in menuItemSelected() with topLevelMenuIndex == -1; we route by id anyway.

    Installed by PluginEditor via juce::MenuBarModel::setMacMainMenu() only when
    wrapperType == Standalone -- in a plugin the host owns the menu bar. macOS asks for each
    top menu's contents every time it's opened, so enable / tick state is always current
    without this model having to observe the document.
*/
class StandaloneMenuBar : public juce::MenuBarModel
{
public:
    explicit StandaloneMenuBar (EditorToolbar& tb) : toolbar (tb) {}

    juce::StringArray getMenuBarNames() override
    {
        return { "File", "Edit", "Tools", "Slice" };
    }

    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String& /*name*/) override
    {
        juce::PopupMenu menu;
        switch (topLevelMenuIndex)
        {
            case 0: toolbar.buildMenuBarMenu (menu, EditorToolbar::ToolsMenuGroup::file);  break;
            case 1: toolbar.buildMenuBarMenu (menu, EditorToolbar::ToolsMenuGroup::edit);  break;
            case 2: toolbar.buildMenuBarMenu (menu, EditorToolbar::ToolsMenuGroup::tools); break;
            case 3: toolbar.buildMenuBarMenu (menu, EditorToolbar::ToolsMenuGroup::slice); break;
            default: break;
        }
        return menu;
    }

    void menuItemSelected (int menuItemID, int /*topLevelMenuIndex*/) override
    {
        if (menuItemID > 0)
            toolbar.performToolsItem (menuItemID);
    }

    // Items for the macOS application ("R3WRK") menu. PluginEditor passes this to
    // setMacMainMenu() as extraAppleMenuItems; JUCE copies it, so it needn't outlive the call.
    juce::PopupMenu getAppleMenuItems() const
    {
        juce::PopupMenu menu;
        toolbar.buildMenuBarMenu (menu, EditorToolbar::ToolsMenuGroup::appMenu);
        return menu;
    }

private:
    EditorToolbar& toolbar;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StandaloneMenuBar)
};
