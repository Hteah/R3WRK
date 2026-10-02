#!/usr/bin/env python3
"""
Builds the printable R3WRK MIDI CC chart (docs/R3WRK MIDI Chart.pdf).

The data comes from the app itself, so the chart can't drift from the code:
    Plugin/build/R3WRKSmokeTest_artefacts/Release/R3WRKSmokeTest --print-midi-chart chart.json
    python3 tools/make_midi_chart.py chart.json "docs/R3WRK MIDI Chart.pdf"
(kMidiCcMap in Plugin/Source/MidiCcMap.h is the source of truth.)
"""
import json
import sys

from reportlab.lib import colors
from reportlab.lib.pagesizes import letter
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import inch
from reportlab.platypus import (KeepTogether, Paragraph, SimpleDocTemplate, Spacer, Table,
                                TableStyle)

# Section order and headings, like a synth manual's MIDI chapter.
SECTIONS = [
    ("Transport", "Transport"),
    ("Knob row", "Knob row (Pitch / Speed / Stretch, Dirt, filter, Start / End)"),
    ("Output", "Output"),
    ("Slot 1", "FX drawer - slot 1 (RTRG / CHO)"),
    ("RTRG", "RTRG - buffer retrig"),
    ("CHO", "CHO - chorus (values are the Monomachine's raw 0-127)"),
    ("Slot 2", "FX drawer - slot 2 (DLY / PLX)"),
    ("DLY", "DLY - delay"),
    ("PLX", "PLX"),
    ("Slot 3", "FX drawer - slot 3 (RVB / SHM)"),
    ("RVB", "RVB - reverb"),
    ("SHM", "SHM - shimmer reverb"),
    ("Overdub", "Overdub"),
    ("Tools", "Tools"),
]
KIND_LABEL = {"knob": "knob", "toggle": "button", "cycle": "button", "trigger": "button"}

INK = colors.HexColor("#202124")
DIM = colors.HexColor("#6b6f76")
RULE = colors.HexColor("#c9ccd1")
BAND = colors.HexColor("#f1f2f4")
ACCENT = colors.HexColor("#a5402d")   # R3WRK's record red


def main(json_path, pdf_path):
    rows = json.load(open(json_path))
    known = {key for key, _ in SECTIONS}
    unknown = sorted({r["section"] for r in rows} - known)
    if unknown:
        sys.exit(f"sections missing from SECTIONS: {unknown}")

    styles = getSampleStyleSheet()
    title = ParagraphStyle("t", parent=styles["Title"], fontName="Helvetica-Bold", fontSize=20,
                           textColor=INK, alignment=0, spaceAfter=2)
    sub = ParagraphStyle("s", parent=styles["Normal"], fontName="Helvetica", fontSize=9,
                         textColor=DIM, leading=12)
    head = ParagraphStyle("h", parent=styles["Normal"], fontName="Helvetica-Bold", fontSize=10,
                          textColor=ACCENT, spaceBefore=8, spaceAfter=3)
    cell = ParagraphStyle("c", parent=styles["Normal"], fontName="Helvetica", fontSize=8,
                          textColor=INK, leading=10)
    note = ParagraphStyle("n", parent=styles["Normal"], fontName="Helvetica", fontSize=8.5,
                          textColor=INK, leading=11.5, spaceAfter=3)

    widths = [0.5 * inch, 1.75 * inch, 0.65 * inch, 4.4 * inch]
    story = [
        Paragraph("R3WRK - MIDI CC Chart", title),
        Paragraph("Every knob and button has a fixed CC number. R3WRK listens on the MIDI channel "
                  "set in Tools &gt; MIDI Channel (default 1, or Omni). Knobs: CC 0-127 covers the "
                  "knob's full travel and CC 64 is exactly its centre. Buttons act when pressed "
                  "(value 64 or more) and ignore the release.", sub),
        Spacer(1, 4),
    ]

    for key, heading in SECTIONS:
        entries = sorted((r for r in rows if r["section"] == key), key=lambda r: r["cc"])
        if not entries:
            continue
        data = [["CC", "Control", "Type", "Range / behaviour"]]
        for r in entries:
            data.append([str(r["cc"]), Paragraph(r["name"], cell), KIND_LABEL[r["kind"]],
                         Paragraph(r["detail"], cell)])
        t = Table(data, colWidths=widths, repeatRows=1)
        t.setStyle(TableStyle([
            ("FONT", (0, 0), (-1, 0), "Helvetica-Bold", 7.5),
            ("TEXTCOLOR", (0, 0), (-1, 0), DIM),
            ("FONT", (0, 1), (0, -1), "Helvetica-Bold", 9),
            ("FONT", (2, 1), (2, -1), "Helvetica", 7.5),
            ("TEXTCOLOR", (2, 1), (2, -1), DIM),
            ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
            ("LINEBELOW", (0, 0), (-1, 0), 0.6, RULE),
            ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, BAND]),
            ("TOPPADDING", (0, 0), (-1, -1), 2),
            ("BOTTOMPADDING", (0, 0), (-1, -1), 2),
        ]))
        story.append(KeepTogether([Paragraph(heading, head), t]))

    story += [
        Paragraph("Setup", head),
        Paragraph("<b>Ableton Live (VST3):</b> on a MIDI track, set MIDI From to your controller and "
                  "MIDI To to the track with R3WRK (pick R3WRK in the second menu). Set the MIDI "
                  "track's Monitor to In (or arm it). Works with R3WRK's window closed.", note),
        Paragraph("<b>Standalone:</b> R3WRK menu &gt; Audio Settings, tick your controller under "
                  "Active MIDI inputs.", note),
        Paragraph("<b>Channel:</b> Tools &gt; MIDI Channel (Standalone: R3WRK menu &gt; MIDI Channel). "
                  "Remembered across sessions.", note),
        Paragraph("<b>Window-only buttons:</b> Scrub, Slice, Clear, Record Desktop and Capture Output "
                  "only respond while R3WRK's window is open.", note),
        Paragraph("Scanning and Live", head),
        Paragraph("<b>Position (CC 28)</b> slides the looping selection through the sample and keeps "
                  "its length: it's the one to scan with, from a controller or an LFO. It has no "
                  "on-screen knob. To slide the loop by hand, turn the on-screen Start knob. "
                  "<b>Start (CC 22)</b> and <b>End (CC 23)</b> only trim their own edge.", note),
        Paragraph("<b>Ableton LFO / Macro / automation:</b> every knob above is also a Live parameter, "
                  "with the same names (e.g. \"DLY Mix\", \"Position\"). To map one: click the LFO's Map, then "
                  "click the parameter in R3WRK's device panel in Live. Turn on Configure and touch a "
                  "knob in R3WRK's window once to add it to that panel. Live can't map a knob turned "
                  "inside any plugin's own window.", note),
    ]

    doc = SimpleDocTemplate(pdf_path, pagesize=letter, title="R3WRK MIDI CC Chart",
                            author="R3WRK", leftMargin=0.6 * inch, rightMargin=0.6 * inch,
                            topMargin=0.55 * inch, bottomMargin=0.55 * inch)

    def footer(canvas, d):
        canvas.saveState()
        canvas.setFont("Helvetica", 7.5)
        canvas.setFillColor(DIM)
        canvas.drawRightString(letter[0] - 0.6 * inch, 0.35 * inch, f"R3WRK MIDI CC Chart - page {d.page}")
        canvas.restoreState()

    doc.build(story, onFirstPage=footer, onLaterPages=footer)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
