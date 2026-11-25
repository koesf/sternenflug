# Sternenflug · Wildcoding

**Sterne, Lichtschweife und selbst gezeichnete Motive als eigene Quelle in OBS Studio.**

Ein Wildcoding-Projekt für kreative Livestreams: Zeichne eine Kontur, löse sie aus und mische sie in den laufenden Sternenflug. Der Effekt entsteht direkt im nativen C++-/Qt-Plugin.

![Sternenflug-Vorschau](docs/sternenflug.png)

## Funktionen

- Sternenflug, Regen und sanfte Abwärtsbewegung.
- Frei gezeichnete Motive mit mehreren Strichen, Rückgängig und erneutem Auslösen.
- Farben, Größenstreuung, Deckkraft, Dichte und Lichtschweife.
- Bewegung, Drift, Wellenformen und grafischer Kurveneditor.
- Effekt-Presets als JSON speichern und austauschen.
- Unabhängige Einstellungen je OBS-Quelle.

## Plattform und Entwicklungsstand

**Native Fassung 0.1.2 · Windows x64 · Direct3D 11.** Das Buildskript verwendet OBS-32.2.2-Header und das offizielle OBS-Qt-Paket 6.11.1. OBS stellt die Laufzeit-DLLs bereit; die Qt-Version muss passen.

Diese Veröffentlichung enthält **Quellcode**. GitHubs „Download ZIP“ enthält keine fertige Plugin-DLL. Andere OBS-/Qt-Kombinationen müssen gesondert geprüft werden. Ein macOS-Build ist in diesem Stand noch nicht vorhanden.

## Einstieg

1. Voraussetzungen und Buildablauf in [BUILD.md](docs/BUILD.md) prüfen.
2. Das Plugin bauen und die enthaltenen Tests ausführen.
3. Das gebaute Plugin gemäß [INSTALLATION.md](docs/INSTALLATION.md) installieren.
4. In OBS unter **Quellen → + → Sternenflug** eine Quelle hinzufügen.
5. In den Quelleneigenschaften **Sternenflug-Steuerung öffnen**, die Zielquelle wählen und im Tab **Zeichnen** ein Motiv erstellen. Mit Rechtsklick oder **Senden** auslösen.

## Projektstruktur

| Pfad | Inhalt |
| --- | --- |
| `native_plugin/src/` | Simulation, Renderer, Qt-Steuerung und OBS-Anbindung |
| `native_plugin/tests/` | Renderer-, Editor- und Plugin-Prüfungen |
| `native_plugin/cmake/` | Einbindung des OBS-Laufzeit-SDKs |
| `native_plugin/scripts/` | Build, Installation und separate Entwicklerprüfungen |
| `docs/` | Anleitung und Vorschau |

## Lizenz

GNU General Public License, Version 2; siehe [LICENSE](LICENSE). OBS Studio und Qt sind separate Abhängigkeiten mit eigenen Lizenzbedingungen.

## Entwicklung

Der Quellcode enthält Tests für Simulation, GPU-Rendering, Qt-Oberfläche und die OBS-Quelle. Die optionalen Portable-/Testszenen-Skripte sind Entwicklerwerkzeuge; für die normale Installation werden sie nicht benötigt. Compilerartefakte, persönliche OBS-Konfigurationen, Protokolle und Zugangsdaten gehören nicht ins Repository.
