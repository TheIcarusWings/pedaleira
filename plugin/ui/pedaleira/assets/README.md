# Pedaleira UI assets

- `HankenGrotesk-*.ttf`: Hanken Grotesk, SIL Open Font License 1.1 (`HankenGrotesk-OFL.txt`).
- `JetBrainsMono-*.ttf`: JetBrains Mono, SIL Open Font License 1.1 (`JetBrainsMono-OFL.txt`).

Both come from the variable fonts in google/fonts (`ofl/hankengrotesk`, `ofl/jetbrainsmono`).
The files here are static instances at the weights the design uses (400, 500, 600 and 700,
500 italic; mono 400 and 500), made with fontTools:

```sh
python3 -m fontTools.varLib.instancer "HankenGrotesk[wght].ttf" wght=600 -o HankenGrotesk-600.ttf
```

JUCE cannot select a weight on a variable font, so the instances are what is embedded.
Neither font declares a Reserved Font Name.

The TONE3000 wordmark is not here: the build embeds `plugin/ui/assets/t3k.svg`, the wordmark
TONE3000 ships with its own open source plugin, as it is (no recolouring). It is TONE3000's
mark, shown only as the attribution its API terms ask for ("Powered by TONE3000", the splash
before sign-in).
