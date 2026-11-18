# NoiseCancel – aktive Geräuschunterdrückung für Android

Die App nimmt Umgebungsgeräusche über das Mikrofon auf und spielt in Echtzeit gegenphasigen Schall („Anti-Schall“) über den Kopfhörer ab, sodass sich beide am Ohr auslöschen. Sie läuft als Vordergrunddienst auch im Hintergrund und bei ausgeschaltetem Bildschirm weiter, bis du sie in der App oder über die Benachrichtigung ausschaltest.

## Installation

1. Unter **Releases** die neueste `NoiseCancel.apk` herunterladen.
2. Öffnen und „Installation aus unbekannten Quellen“ erlauben.
3. App starten, Mikrofon- und Benachrichtigungsberechtigung erlauben.

Jeder Push auf `main` baut die App automatisch über GitHub Actions neu (`.github/workflows/build.yml`) und veröffentlicht eine neue Release-Version.

## Bedienung

- **Starten/Stoppen:** großer runder Knopf. Danach kann die App geschlossen werden.
- **Stärke:** Lautstärke des Gegenschalls (0,5 dB pro Schritt, 40 % = 0 dB). Langsam erhöhen, bis das Brummen am leisesten ist. Zu viel macht es wieder lauter.
- **Feinabstimmung Phase:** ±20 ms. Gleicht Laufzeiten aus, die das Handy nicht messen kann (Wandler, Abstand Mikrofon–Ohr).
- **Analyse:** zeigt, wie viel des aktuellen Geräuschs vorhersagbar und damit auslöschbar ist, die gemessene Audio-Latenz und den Audiopfad.

## Technik

| Baustein | Umsetzung |
|---|---|
| Audio-I/O | AAudio (NDK), Full-Duplex, `LOW_LATENCY` + `EXCLUSIVE` (MMAP, falls verfügbar), Mikrofon-Preset `UNPROCESSED` (ohne AGC und Rauschfilter) |
| Latenzmessung | Hardware-Zeitstempel von Ein- und Ausgabestream → exakte Gesamtlatenz Mikrofon → Kopfhörer, Median über 15 Messungen |
| Signalweg | DC-Blocker (1 Hz) → linearphasiger Blackman-FIR-Tiefpass (1,3 kHz) + Dezimierung auf ≈ 4 kHz → **NLMS-Prädiktor** (128 Taps) → Polyphasen-Interpolation → Feinverzögerung → Invertierung × Stärke → Soft-Limiter (max. −6 dBFS) |
| Latenzkompensation | Der Prädiktor sagt das Geräusch genau um *Latenz + Filterlaufzeit + Feinabstimmung* voraus; der Rest unterhalb eines Abtastwerts wird mit Gerätefrequenz-Verzögerung ausgeglichen |
| Sicherheit | Rückkopplungserkennung (plötzlicher Pegelsprung > 15 dB → stumm und neu lernen), Pegelbegrenzung, NaN-Schutz, automatischer Neustart bei Gerätewechsel |
| Hintergrund | Foreground-Service Typ `microphone` + Partial Wakelock |

### Warum Vorhersage statt einfach invertieren?

Kommerzielle ANC-Kopfhörer haben Latenzen im Mikrosekundenbereich und ein Fehler-Mikrofon im Ohr. Ein Handy hat 10–40 ms Audio-Latenz (Bluetooth 100–300 ms). In dieser Zeit ist der aufgenommene Schall längst am Ohr vorbei. Ein einfach invertiertes Signal käme also zu spät und würde das Geräusch eher verstärken.

Gleichmäßige Geräusche (Lüfter, Motoren, Netzbrummen) sind aber periodisch und damit vorhersagbar. Der adaptive Prädiktor lernt sie und berechnet, wie das Geräusch in *Latenz* Millisekunden klingen wird. Zufällige Geräusche wie Stimmen oder Rauschen sind nicht vorhersagbar; dafür gibt der Filter automatisch keinen Gegenschall aus, macht also nichts lauter.

### Simulation

`test/sim.cpp` testet den DSP-Kern ohne Handy (läuft bei jedem Build mit):

```
g++ -O2 -std=c++17 test/sim.cpp -o sim && ./sim
```

Ergebnis (Pegel am Ohr, Bereich < 1 kHz):

```
Toene, 25 ms Latenz (Kabel)                -> -34 dB
Toene, 180 ms Latenz (Bluetooth)           -> -34 dB
Toene + starkes Rauschen, 25 ms            -> -21 dB
Toene, 25 ms, 0.3 ms Messfehler            -> -12 dB   (dafür gibt es die Feinabstimmung)
Toene, 25 ms, Uebersprechen 20 %           -> -15 dB
Nur Rauschen (darf nicht lauter werden)    ->  +0 dB
Lautsprecher, Rueckkopplung 150 %          ->  -3 dB   (stabil, kein Aufschaukeln)
```

In der Praxis ist die Wirkung kleiner als in der Simulation, weil das Handy-Mikrofon nicht im Ohr sitzt und Wandler und Kopfhörer eigene Phasengänge haben. Realistisch sind einige dB bei gleichmäßigem Brummen.

## Hinweis zur Signatur

`app/noisecancel.jks` ist ein eigens erzeugter Schlüssel, damit neue Builds als Update installiert werden können. Er ist nur für privates Sideloading gedacht.
