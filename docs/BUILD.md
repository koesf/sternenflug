# Windows-Build

## Voraussetzungen

- Windows x64 mit Direct3D 11.
- Visual Studio 2022 oder 2026 mit C++-x64-Werkzeugen, Windows SDK und CMake ab 3.28.
- Eine passende OBS-x64-Laufzeit mit `obs.dll`, `obs-frontend-api.dll` und Qt 6.11.1.
- Internetzugang für die offiziellen OBS-Header und das OBS-Qt-Entwicklungspaket.

`native_plugin/scripts/Build.ps1` lädt OBS-32.2.2-Header und das offizielle Qt-6.11.1-Paket herunter und prüft die festgelegten SHA-256-Werte. Es erzeugt Importbibliotheken aus den vorhandenen OBS-DLLs. Die installierte OBS-Anwendung wird dadurch nicht verändert.

## Bauen

Vom Stammordner des Projekts in PowerShell:

```powershell
.\native_plugin\scripts\Build.ps1 -ObsRoot 'C:\Program Files\obs-studio' -Configuration Release
```

Bei einer anderen OBS-Installation `-ObsRoot` entsprechend angeben. Das Standardziel ist die normale Windows-Programminstallation. Der Parameter benennt den Ordner oberhalb von `bin\64bit`.

Downloads und Buildartefakte landen standardmäßig in `%USERPROFILE%\.cache\sternenflug-native`. Mit `-CacheRoot` und `-BuildRoot` lassen sich andere lokale Arbeitsordner wählen. Diese Dateien werden nicht ins Repository aufgenommen.

Weitere Optionen: `-ConfigureOnly`, `-SkipTests` und `-Configuration RelWithDebInfo`. Für weitergegebene Binärpakete keine ungeprüften Debugsymbole oder PDB-Dateien aufnehmen.

## Ergebnis und Tests

Das Pluginpaket liegt unter:

```text
<CacheRoot>/build/package/sternenflug/
  bin/64bit/sternenflug.dll
```

Ein optional vorhandener Ordner `data/` gehört ebenfalls zum Paket. Der aktuelle Renderer enthält seinen Shader im Quellcode und benötigt keine zusätzliche Shaderdatei.

Der normale Build führt diese vorhandenen CTest-Ziele aus:

| Test | Prüft |
| --- | --- |
| `sf_core_tests` | Kernsimulation |
| `sf_renderer_smoke` | D3D11-Renderer und transparente Ausgabe |
| `sf_editor_smoke` | Qt-Widgets, Zeichnen, Kurven und Presets |
| `sf_plugin_smoke` | Tatsächliches Plugin und mehrere OBS-Quellen |

GPU-Tests benötigen eine funktionierende D3D11-Umgebung. Der Editor-Test verwendet das Qt-Plattformplugin `minimal`.

`Test-Portable.ps1` und `Setup-TestScene.ps1` können eine getrennte portable OBS-Testinstallation vorbereiten. `Benchmark.ps1` ist ein optionales Messwerkzeug. Diese Skripte sind keine Voraussetzung für die normale Nutzerinstallation; vorhandene persönliche OBS-Profile werden für die Tests nicht benötigt.

## Offizielle Abhängigkeiten

- [OBS Studio 32.2.2](https://github.com/obsproject/obs-studio/releases/tag/32.2.2)
- [OBS Qt-Paket 2026-07-15](https://github.com/obsproject/obs-deps/releases/tag/2026-07-15)
- [OBS Plugin Template](https://github.com/obsproject/obs-plugintemplate)
