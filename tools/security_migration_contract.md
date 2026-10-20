# Offline-Vertrag für den Security-Migrationsbaustein

Dieser Baustein verarbeitet ausschließlich bereits **logisch extrahierte** Hostdateien. Er liest kein Gerät, kein NVS-Partitionsabbild und kein LittleFS-Rohabbild. Er montiert oder beschreibt auch keine Zielpartition. `export` erstellt ein externes, nur für den eigenen Benutzer lesbares Paket; `import-stage` prüft dieses Paket erneut und erstellt eine ebenso geschützte Host-Stagingmappe. Beide Zielpfade müssen außerhalb des Repositories liegen und dürfen noch nicht existieren. Die jeweilige Elternmappe muss dem Benutzer gehören und Modus 0700 haben. Quelldateien, Paketdateien und Stagingdateien müssen Modus 0600 haben; alle Unterverzeichnisse Modus 0700. Das Paket ist **nicht verschlüsselt**. Seine SHA-256-Werte erkennen versehentliche Veränderungen, authentisieren aber keine Quelle und ersetzen keinen geeigneten geschützten Datenträger.

## Logischer Eingangsbaum

Der zulässige Quellbaum enthält `source-inventory.json` mit exakt den beiden sortierten Pfadlisten `nvs` und `littlefs`. Diese Liste muss ein späterer, separat zu prüfender Alt-Firmware-Exporter aus **allen tatsächlich vorhandenen** NVS-Schlüsseln und LittleFS-Dateien bilden. Fehlende, zusätzliche oder unbekannte Pfade sperren den Baustein. Die aktuelle Hostprüfung kann nicht selbst beweisen, dass ein künftiger Extraktor keinen alten Schlüssel übersehen hat.

| Logischer Pfad | Aktueller Speicher- und Übernahmevertrag |
| --- | --- |
| `nvs/katmed/state` | Erforderlicher versionierter Store-Blob. Der produktive v11-Decoder prüft ihn und der Encoder erstellt ein kanonisches v11-Ziel mit maximal 4096 Byte. Damit bleiben App-Einstellungen, Provisionierung, bis zu vier geordnete WLAN-Profile, bis zu 64 Mesh-Namen, Pending/Konflikte, Bindung, historische Projektion und die persistierte Sprache erhalten. Für ältere Blobs ohne Sprachfeld setzt die semantische Umwandlung English als vorhandenen Produktdefault. Das Paket enthält den unveränderten Quellblob **und** den v11-Zielblob. |
| `nvs/katmed/mesh_history` | Optionaler eigener Mesh-Historien-Blob. Der produktive Decoder prüft Format, Gruppe, Einträge und CRC. Seine Originalbytes bleiben erhalten; ein historischer v1-Blob wird hier nicht stillschweigend umgeschrieben. |
| `nvs/katmed_web/auth` | Optionaler Web-Auth-Blob. Länge, Magic, Version, Zertifikatlänge und ungenutzter Zertifikatspuffer werden strukturell geprüft. Verifier und Zertifikat werden bytegleich erhalten. Keine Passwort- oder Zertifikatsauthentisierung wird daraus abgeleitet. |
| `nvs/katmed_web/identity` | Optionale Serveridentität. Länge, Magic und Schlüssel-/Zertifikatlängen werden strukturell geprüft und die Originalbytes erhalten. Die spätere Laufzeit muss das Schlüssel-Zertifikat-Paar mit `katmed_web_identity_load_or_create()` beziehungsweise dessen Validierung gesondert prüfen. |
| `nvs/katmed_reset/journal` | Optionales zweibytes Resetjournal. Ein laufender Reset sperrt die Migration; ein ruhender Zustand wird erhalten. |
| `littlefs/user-images/img-XXXXXXXX.jpg`, `cat-XX-[01].jpg` | Nur benannte Nutzerbilder und vorhandene Katalogkopien werden bytegleich erhalten. Abgebrochene Upload-Dateien und unbekannte Namen sperren den Export. Die spätere Laufzeit muss JPEG, IDs und tatsächlichen LittleFS-Füllstand prüfen. |
| `littlefs/acme/` | Sechs von der Anwendung konfigurierte ACME-Dateinamen werden bytegleich erhalten, falls vorhanden. Der gepinnte ACME-Client kann bei erfolgreicher ARI-Abfrage zusätzlich `/katmed/acme/renewal.json` speichern; diese Datei bleibt in Export, Host und Import gesperrt. Der aktuelle öffentliche Resetpfad berücksichtigt sie bereits, was die fehlende gemeinsame Migrationskette nicht ersetzt. Die ACME-Bibliothek ist in der laufenden Firmware noch nicht aktiviert. |

„Optional“ bei `mesh_history` bedeutet ausschließlich, dass dieser Pfad im Inventar fehlen darf. Ist er inventarisiert, muss sein Blob eine der produktiven Mesh-Codec-Längen (2080 oder 2152 Byte) und gültigen Inhalt besitzen. Ein deklarierter leerer oder zu kurzer Blob wird schon beim Host-Export und bei `import-stage` abgewiesen. Der Firmwareparser prüft die Länge nochmals vor Stufe, Journal und Live-Key; ein CRC-konsistentes Capsule macht eine leere History nicht gültig.

Weitere NVS-Namespace-Schlüssel, insbesondere mögliche alte Wi-Fi-Treiberwerte oder `katmed_acme`-Schlüssel, werden **nicht** übernommen. Wenn ein vollständiger Extraktor sie findet, sperrt dieser Vertrag und verlangt eine ausdrückliche semantische Zuordnung. Der aktuelle Anwendungscode führt die priorisierten WLAN-Profile im `katmed/state`-Blob; SDK-Treiberreste dürfen nicht als gleichwertige Profile ausgegeben werden. Frei editierbare Nutzertexte außerhalb der in diesem Blob enthaltenen Werte sind im aktuellen Store-Schema nicht vorhanden. Einkompilierte private UI-Defaults gehören nicht zu diesem Datenspeichervertrag.

## Ablauf und noch fehlende Geräteschnittstelle

### Lesender produktiver Quelladapter (dieser Folgebaustein)

`firmware/components/katmed_security/katmed_source_export.c` implementiert
`katmed_source_export()` gegen die **produktive** ESP-IDF-NVS-API und das bereits
gemountete `user_images`-LittleFS. Die Funktion ist derzeit nicht an eine
Laufzeitaktion gebunden. Sie initialisiert NVS nicht, mountet nicht, formatiert
nicht, verändert keine Quelldatei und schreibt weder Werte noch Pfade in Logs.
Der spätere Aufrufer muss alle Store-, Web-, Mesh-, Reset-, Bild- und ACME-Schreiber
für die gesamte Dauer anhalten und vor dem Aufruf einen lesbaren, unverändert
gemounteten Altzustand sicherstellen. Die heutige Firmware garantiert diese
Quieszenz noch nicht. Ein partiell empfangener Lauf ist nur mit `finish(true)`
verwendbar; bei `finish(false)` muss der Empfänger alle empfangenen Bytes
verwerfen. Die Funktion übergibt logische Pfade, Längen und Bytes an vier
Callbacks (`begin`, `bytes`, `end`, `finish`). Ein externer Transport und ein
Empfänger, der daraus geschützte Dateien sowie die exakt sortierten Listen in
`source-inventory.json` schreibt, fehlen noch. Das ist die bewusste
Adaptergrenze zum vorhandenen Hostpaket, keine fertige Geräteexportfunktion.

Der Adapter inventarisiert **alle** sichtbaren Einträge der Standard-NVS-Partition
mittels `nvs_entry_find("nvs", NULL, NVS_TYPE_ANY, ...)`. Er übernimmt nur die
fünf unten benannten Blobschlüssel. Ein weiterer Schlüssel, eine andere
NVS-Typkennung oder ein fehlender `state`-Blob sperrt den gesamten Lauf, sodass
zum Beispiel SDK-WLAN-Reste und `katmed_acme` nicht stillschweigend verschwinden.
Die bekannten Werte werden mit `NVS_READONLY` und `nvs_get_blob()` gelesen.
Der ruhende Resetjournalzustand wird vorausgesetzt und geprüft. Der Adapter
inventarisiert danach das LittleFS-Wurzelverzeichnis sowie `user-images` und
`acme`; nur die vereinbarten Dateinamen und reguläre Dateien sind zulässig.
Das fehlende optionale `acme`-Verzeichnis ist erlaubt. Bei vorhandenem Sink mit
`finish` führt jeder Fehler, auch ein fehlender `begin`-, `bytes`- oder
`end`-Callback, genau einmal zu `finish(false)`, ohne NVS oder LittleFS zu
löschen oder zu ändern. Für `sink == NULL` oder fehlendes `finish` ist kein
Abschlusscallback möglich; die Funktion gibt unmittelbar `false` zurück.

Bei der Dateitypprüfung verwendet der Adapter unter ESP-IDF 6.1 `stat()` und
`fstat()`; dessen VFS stellt kein `lstat()` bereit und LittleFS kennt keine
Symlinks. Auf dem Host verwendet dieselbe Quelle `lstat()`, sodass ein
synthetischer Symlink auch bei zulässigem Dateinamen abgewiesen wird. Nach
jedem neutralen C3-/C6-Sicherheitsbuild kann
`python3 tests/host_source_export/check_target_link.py c3` beziehungsweise
`python3 tests/host_source_export/check_target_link.py c6` den bislang
unaufgerufenen Adapter mit `-Wl,-u,katmed_source_export` in einem temporären
ELF/Map-Link erzwingen und das Symbol im Ergebnis prüfen.

| Kategorie | Produktiver Speicherort und Quellpfad |
| --- | --- |
| WiFi-Prioritäten, Alarm- und Appzustand, App-Historie, Mesh-Bindung, Namen, Pending und Konflikte, gespeicherte nutzereigene Texte sowie Sprache im Store | `katmed_store_idf.c` liest/schreibt `nvs/katmed/state`; der Quelladapter liefert die Originalbytes und der Host-Codec prüft das vorhandene Store-Schema bis v11 und die kanonische v11-Ausgabe. Für v10 ohne Sprachbyte wird English übernommen; die Firmware akzeptiert nur v11-Capsules. |
| Gesonderte Mesh-Historie | `katmed_store_idf.c` liest/schreibt `nvs/katmed/mesh_history`; der Host-Codec prüft den v1/v2-Blob. |
| Web-Authentisierung und hinterlegtes Client-Zertifikat | `katmed_web.c` liest/schreibt `nvs/katmed_web/auth`; der Hostvertrag prüft Form und Version. |
| Web-Serveridentität samt privatem Schlüssel und Zertifikat | `katmed_web_identity.c` liest/schreibt `nvs/katmed_web/identity`; der Hostvertrag prüft Form und Version. Die spätere Runtime muss das Schlüsselpaar gesondert prüfen. |
| Resetjournal | `katmed_reset_idf.c` liest/schreibt `nvs/katmed_reset/journal`; ein laufender Reset sperrt die Ausgabe. |
| Hochgeladene Nutzerbilder und vorhandene Katalogkopien | `katmed_image_store_idf.c` verwendet `/katmed/user-images`; der Adapter liefert vorhandene `img-XXXXXXXX.jpg` und `cat-XX-[01].jpg` bytegleich. |
| ACME-Zustand, falls die derzeit nicht aktive Bibliothek ihn gespeichert hat | `katmed_acme.cpp` bindet `/katmed/acme` ein; die sechs konfigurierten Dateien sind zugelassen. Ein vorhandenes `renewal.json` aus dem ARI-Pfad sperrt bewusst vor der Ausgabe. |

Die Quellgrenzen sind höchstens 4096 Byte pro NVS-Blob, 128 KiB pro Datei,
höchstens 128 logische Einträge und weniger als 256 KiB Nutzdateiinhalt mit
8 KiB Reserve. Das sind **Vorprüfungen**, keine Garantie, dass ein neuer
24-KiB-NVS-Bereich oder das 256-KiB-LittleFS samt Metadaten passt. Die
Versions- und CRC-Prüfung des Store- und Mesh-Inhalts findet im anschließenden
unveränderten produktiven Host-Codec statt. Bei einer während des Lesens
ändernden Quelle ist kein konsistenter Snapshot zugesichert; deshalb ist die
Schreibsperre des späteren Aufrufers zwingend.

**Noch nicht angeschlossen beziehungsweise ausdrücklich offen:** Ein
geräteeigener Aufruf, ein authentisierter und vertraulicher Transport, ein
empfangsseitiges atomisches Anlegen der externen logischen Dateien und
`source-inventory.json`, ein vollständiger Firmware-Importer mit Rücklesen/Journal,
Kapazitätsnachweis auf echten Partitionen sowie Geräte- und Recoveryfreigabe.
Ein SDK-WLAN-NVS-Eintrag oder ein `katmed_acme`-NVS-Eintrag würde derzeit
bewusst sperren; für solche tatsächlich gefundenen Schlüssel muss vor der
Migration eine eigene semantische Zuordnung beschlossen und implementiert
werden. Abgebrochene `.upload-*.tmp`-Dateien sperren als nicht zuordenbarer
Quellrest. Frei editierbare Texte außerhalb des Store-Blobs wurden in den
aktuellen Persistenzaufrufen nicht gefunden; einkompilierte private UI-Defaults
und flüchtige Sitzungen sind keine gespeicherten Laufzeitdaten.
Der aktuelle ACME-Dateireset umfasst `renewal.json` bereits. Vor einer
ACME-Aktivierung müssen nun auch Quelladapter, Hostpaket und Importer diesen
ARI-Zustand gemeinsam übernehmen oder ausdrücklich anders semantisch entscheiden
und prüfen. Ein bloßes Zulassen des siebten Namens wäre kein vollständiger Vertrag.

### Externes Hostpaket und begrenzter Import

Der Hostaufruf ist `python3 tools/security_migration.py export --source <geschützter-logischer-altbaum> --output <neues-externes-paket>` und danach `python3 tools/security_migration.py import-stage --source <paket> --output <neue-externe-stagingmappe>`. Die Befehle geben nur einen generischen Status aus. Sie dürfen keine echten Daten auf der Konsole oder in CI ausgeben. Ein Fehler lässt den Altbaum und ein bereits vollständiges Paket bestehen; ein neuer Zielname erscheint nur nach vollständigem Schreiben. Die Veröffentlichung nutzt unter Linux `renameat2` mit `RENAME_NOREPLACE`: Wird der Zielname nach der Vorprüfung konkurrierend belegt, schlägt die Aktion fehl und entfernt ihre temporäre Stagingmappe. Ist diese Operation nicht verfügbar, erfolgt keine Veröffentlichung. Ein begrenztes Host-Budget und die 4096-Byte-v11-Codecgrenze führen bei Platzmangel zu Abweisung statt Verkleinerung.

Für einen wirklichen Cutover fehlt weiterhin der **vollständige, aufrufbare alte Firmware-Exporter**: Der vorhandene lesende Adapter muss nach einem nachgewiesen ruhenden Resetzustand und stillgesetzten Schreibern sicher an einen Transport und externen Empfänger gebunden werden. Sein Ergebnis muss vor dem Tabellenwechsel extern gesichert und unabhängig gegen diese Paketprüfung verglichen werden. Das bestehende `katmed_nvs_init()` lässt einen belegten unmarkierten NVS-Bereich bewusst nicht als Migration durch. Die im Host erzeugte Stagingmappe ist **keine** geflashte oder gelinkte Runtime-Funktion.

#### Produktiver Teilslice

`import-stage` schreibt zusätzlich `firmware-import-v1.bin` in die geschützte externe Stagingmappe. Dessen `KMI1`-Header bindet Version 1, Gesamtlänge, Eintragszahl und CRC32 aller folgenden Records; jeder Record trägt Pfadlänge, Datenlänge, CRC32 und die logischen Pfad-/Datenbytes. Die Hostseite erstellt ihn **erst nach** Inventur, SHA-256-Manifestprüfung und Prüfung der produktiven Store-/Mesh-Codecs. Der Firmwarekern in `katmed_import.cpp` prüft den gesamten Capsuleinhalt noch einmal, bevor er einen Zielschlüssel schreibt: Format, Integrität, bekannte und eindeutige Pfade, Längen, höchstens 128 Einträge, höchstens 192 KiB LittleFS-Inhalt mit 64 KiB konservativer Metadatenreserve sowie ein kanonisches v11-Store-Blob einschließlich Sprachbyte und optional die durch den produktiven Mesh-Codec geprüfte `mesh_history`. Ein direktes v10-Capsule wird vor Mutation abgewiesen; die semantische v10→v11-Umwandlung gehört in den geprüften Hostpaketpfad. CRC32 erkennt Beschädigung; weder CRC noch Paket-SHA authentisieren einen Absender.

Das Capsule dupliziert die logischen Werte innerhalb der externen, geschützten Stagingmappe. `--max-stage-bytes` begrenzt weiterhin die Summe der logischen Zielwerte und ist keine Zusage über den gesamten externen Plattenverbrauch. Ein Schreib-/Platzfehler verhindert die atomare Veröffentlichung der Stagingmappe.

**Aktuell übernehmbar ist nur ein Paket, dessen vollständiges Inventar genau `nvs/katmed/state` und optional `nvs/katmed/mesh_history` enthält.** Alle anderen bekannten NVS- und LittleFS-Klassen werden vollständig auf Pfad, Länge und CRC geprüft, aber vor der ersten Zielmutation ausdrücklich abgewiesen. Ihre eigene Fachsemantik wird erst vom späteren Zieladapter geprüft. Ein unbekannter Pfad einschließlich `littlefs/acme/renewal.json` wird ebenfalls abgewiesen. Ein übliches Paket mit Bildern, Web- oder ACME-Werten ist daher noch **nicht importierbar** und wird nicht stillschweigend verkleinert.

`katmed_import_apply_idf()` ist ein bewusst noch unverdrahteter Produktaufruf für einen späteren geschützten Empfänger. Der Aufrufer muss nach der gesicherten NVS-Initialisierung ein stabiles, vollständiges Capsule über den `KatmedImportInput.read`-Callback und die lokal gelesene Device-ID als `expected_device` bereitstellen, alle Schreiber anhalten und vor dem normalen Store-/Web-/Mesh-Start aufrufen. Der IDF-Adapter vergleicht diese ID erneut mit der read-only Factory-MAC. Eine fremde Device-ID im Store-Blob sperrt vor der ersten Nutzdatenmutation. Im neutralen Profil verweigert der IDF-Adapter jeden Import. Im Security-Profil prüft er die leere Ziel-NVS-Inventur und `nvs_get_stats()` mit Reserve für Stufe, Zielwerte und Journal und schreibt dann die Stufe in `katmed_imp/stage`. Nach Rücklesen schreibt er ein CRC-gebundenes `katmed_imp/journal`. Erst danach kopiert er `state` und gegebenenfalls `mesh_history` in die produktive `katmed`-Namespace und liest beide zurück. Bei Neustart liest `katmed_import_resume_idf()` die lokale Factory-MAC unabhängig von Capsule und Journal. Der generische Replay prüft die Stufe erneut per CRC und Codec und bindet die darin gespeicherte Device-ID an diese lokale ID, bevor er einen Live-Key schreibt oder Journal beziehungsweise Stufe löscht. Auch die Aufräumstufe ohne Journal unterliegt dieser Prüfung. Der Secure-Bootpfad ruft Replay nach `katmed_nvs_init()` und vor Diensten auf und hält bei ID-Abweichung, beschädigtem Journal, fehlender Stufe oder unvollständiger Kopie an. Ein vor Journalbildung verwaister Stufenwert hält ebenfalls den Boot an und kann nur mit `katmed_import_abort()` verworfen werden; nach erfolgreich verifiziertem Ziel und entferntem Journal ist eine noch vorhandene Stufe beim nächsten Boot aufräumbar. NVS-Commit und Rücklesen sind die tatsächliche Kapazitätsentscheidung; die Entry-Zahl ist eine konservative Vorprüfung und kein Flash-/Wear-Nachweis.

**Noch offen:** Transport und geschützter Empfänger für das Capsule, Quieszenz der Alt- und Neuschreiber, Katalogbindung, produktive Zieladapter und gemeinsame Wiederanlaufsemantik für `katmed_web/auth` und `identity`, Resetjournal, Nutzerbilder sowie alle ACME-Dateien. Für LittleFS fehlen insbesondere tatsächliches freies Volumen, atomare Dateiablage, Verzeichnis-/Metadatenreserve unter realen Schreiblasten und Rücklesen. Die sechs konfigurierten ACME-Dateien sind noch kein vollständiger ARI-Vertrag. Ohne diese Anschlüsse und eine gerätegebundene Recoveryprobe ist der Adapter keine Migrations- oder Sicherheitsfreigabe.

`katmed_import_abort_idf()` stellt den begrenzten Abbruch für eine Stufe ohne Journal und ohne produktive Zielkeys bereit. Ein ausstehendes Journal lässt sich bewusst nicht abbrechen; es muss aus der geprüften Stufe wiederaufgenommen werden. Für den Abbruch existiert noch keine Nutzeraktion oder Transportsteuerung.

Das Layout verlegt Teile von NVS und Schlüsselbereich in die frühere Factory-App. Weder ein rohes Alt-NVS-Image noch eine alte Partitionstabelle oder ein gewöhnlicher Klartext-Flash sind daher ein zulässiger Import. Der spätere Release-Mode-Update-/Recoveryweg, individuelle Schlüssel, die allgemeine NVS-Integritätsgrenze und echte Geräteabnahme bleiben gesondert offen. Der veröffentlichte ACME-Dateireset einschließlich `renewal.json` bleibt erhalten; dessen Zusammenspiel mit einer vollständigen Export-/Importkette ist noch nicht belegt. Der Gerätebereitschaftsstatus lautet „Noch nicht bereit“.

Primärquellen: [ESP-IDF 6.1 NVS Encryption](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/storage/nvs_encryption.html), [NVS-API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/storage/nvs_flash.html) und [Flash Encryption/Update-Grenzen](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/security/flash-encryption.html). Die Hostprüfung bindet außerdem die unveränderten Quellcodecs `katmed_store.cpp` und `katmed_mesh_status.cpp` ein.
