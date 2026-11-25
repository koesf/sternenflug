# Installation und Bedienung

Diese Quellcode-Veröffentlichung enthält keine vorkompilierte DLL. Zuerst den [Build](BUILD.md) ausführen oder ein separat gebautes, zur eigenen OBS-/Qt-Version passendes Pluginpaket verwenden.

1. Laufende Aufnahmen und Streams beenden und OBS schließen.
2. Den gebauten Paketordner bestimmen: `<CacheRoot>\build\package\sternenflug`.
3. Vom Projektordner aus in PowerShell installieren:

   ```powershell
   .\native_plugin\scripts\Install.ps1 -PackageRoot '<CacheRoot>\build\package\sternenflug'
   ```

   `<CacheRoot>` durch den eigenen Build-Cache ersetzen. Standardziel ist `%PROGRAMDATA%\obs-studio\plugins\sternenflug`; dafür können erhöhte Schreibrechte nötig sein. Mit `-InstallRoot` ist ein anderer geeigneter Pluginpfad wählbar.

4. OBS starten und unter **Quellen → + → Sternenflug** eine Quelle hinzufügen.
5. In den Quelleneigenschaften **Sternenflug-Steuerung öffnen** wählen.

Der Installer prüft die kopierte DLL mit SHA-256. Bei einer vorhandenen Installation legt er vor dem Überschreiben eine Sicherung an. In Gebrauch befindliche Plugin-DLLs werden nicht ersetzt.

## Motive zeichnen

- Im Dock die gewünschte Sternenflug-Quelle auswählen.
- Im Tab **Zeichnen** mit der linken Maustaste zeichnen. Ein Motiv kann mehrere Striche enthalten.
- Mit Rechtsklick oder **Senden** auslösen.
- Einmischdauer und Symbolanteil bestimmen, wie lange und wie häufig neue Motive erscheinen. Bereits gestartete Motive fliegen zu Ende.

Farbe, Größe, Schweife, Bewegungsrichtung und zeitliche Verläufe werden über die übrigen Tabs eingestellt. Presets lassen sich als JSON austauschen; eigene Dateien vor dem Teilen auf persönliche Inhalte prüfen.

## Entfernen

OBS schließen. Bei der Standardinstallation ausschließlich den Pluginordner `%PROGRAMDATA%\obs-studio\plugins\sternenflug` entfernen oder zunächst an einen anderen Ort sichern. Bei einem abweichenden Installationsziel den tatsächlich verwendeten Pluginordner nehmen. OBS-Profile und Szenensammlungen werden dafür nicht gelöscht; Szenen mit einer Sternenflug-Quelle benötigen das Plugin weiterhin.
