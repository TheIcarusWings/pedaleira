# Pedaleira listening test

A blind listening test for Pedaleira's amp models. Open the site, listen to the
reference and the numbered clips, and score how close each one sounds.

- `index.html`: the test page (static, no build step).
- `config.js`: the round name and where results are sent.
- `trials.json`: the rounds. Each has a reference and clips with opaque ids;
  which clip is which is not published.
- `clips/`: the audio (AAC in MP4).
