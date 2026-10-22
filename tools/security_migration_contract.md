# Offline-Vertrag für den Security-Migrationsbaustein

Dieser Baustein verarbeitet ausschließlich bereits **logisch extrahierte** Hostdateien. Er liest kein Gerät, kein NVS-Partitionsabbild und kein LittleFS-Rohabbild. Er montiert oder beschreibt auch keine Zielpartition. `export` erstellt ein externes, nur für den eigenen Benutzer lesbares Paket; `import-stage` prüft dieses Paket erneut und erstellt eine ebenso geschützte Host-Stagingmappe. Beide Zielpfade müssen außerhalb des Repositories liegen und dürfen noch nicht existieren. Die jeweilige Elternmappe muss dem Benutzer gehören und Modus 0700 haben. Quelldateien, Paketdateien und Stagingdateien müssen Modus 0600 haben; alle Unterverzeichnisse Modus 0700. Das Paket ist **nicht verschlüsselt**. Seine SHA-256-Werte erkennen versehentliche Veränderungen, authentisieren aber keine Quelle und ersetzen keinen geeigneten geschützten Datenträger.

## Logischer Eingangsbaum

Der zulässige Quellbaum enthält `source-inventory.json` mit exakt den beiden sortierten Pfadlisten `nvs` und `littlefs`. Diese Liste muss ein späterer, separat zu prüfender Alt-Firmware-Exporter aus **allen tatsächlich vorhandenen** NVS-Schlüsseln und LittleFS-Dateien bilden. Fehlende, zusätzliche oder unbekannte Pfade sperren den Baustein. Die aktuelle Hostprüfung kann nicht selbst beweisen, dass ein künftiger Extraktor keinen alten Schlüssel übersehen hat.

| Logischer Pfad | Aktueller Speicher- und Übernahmevertrag |
| --- | --- |
| `nvs/katmed/state` | Erforderlicher versionierter Store-Blob. Der produktive v11-Decoder prüft ihn und der Encoder erstellt ein kanonisches v11-Ziel mit maximal 4096 Byte. Damit bleiben App-Einstellungen, Provisionierung, bis zu vier geordnete WLAN-Profile, bis zu 64 Mesh-Namen, Pending/Konflikte, Bindung, historische Projektion und die persistierte Sprache erhalten. Für ältere Blobs ohne Sprachfeld setzt die semantische Umwandlung English als vorhandenen Produktdefault. Das Paket enthält den unveränderten Quellblob **und** den v11-Zielblob. |
| `nvs/katmed/mesh_history` | Optionaler eigener Mesh-Historien-Blob. Der produktive Decoder prüft Format, Gruppe, Einträge und CRC. Seine Originalbytes bleiben erhalten; ein historischer v1-Blob wird hier nicht stillschweigend umgeschrieben. |
| `nvs/katmed_web/auth` | Optionaler echter `KatmedWebAuthRecord` (1112 Byte). Wenn vorhanden, ist `identity` Pflicht: kein stiller Identitätswechsel für ein eingerichtetes Konto. Host und Firmware prüfen Form, Version, Zertifikatlänge, Fingerprint und ungenutzte Bytes; Verifier und Zertifikat bleiben bytegleich. Der IDF-Zieladapter prüft ein vorhandenes Client-Zertifikat mit dem Produkt-X.509-Parser. |
| `nvs/katmed_web/identity` | Optionale echte `KatmedWebIdentity` (1288 Byte); vor der Kontoeinrichtung darf sie allein existieren. Host und Firmware prüfen Form, Längen und ungenutzte Bytes. Vor Zielmutation und Replay prüft der IDF-Adapter das private Schlüssel/Zertifikat-Paar mit der produktiven `identity_valid()`-Logik. |
| `nvs/katmed_reset/journal` | Optionales zweibytes Resetjournal. Ein laufender Reset sperrt die Migration; ein ruhender Zustand wird erhalten. |
| `nvs/netif_ip_info/WIFI_AP_DEF`, `WIFI_STA_DEF` und ihre `_check`-Keys | Bis zu zwei IP-Blobs zu je 12 Byte und zwei U8-Konfliktprüfwerte der gepinnten IoT Bridge 1.0.1 werden unter ihren exakten Namen erhalten. Jeder dieser vier Einträge ist einzeln optional; ein Prüfwert kann nach Löschen des zugehörigen IP-Blobs allein verbleiben. Die Werte sind Interfacezustand und keine WLAN-Profile. |
| `littlefs/user-images/img-XXXXXXXX.jpg`, `cat-XX-[01].jpg` | Benannte Nutzerbilder und vorhandene Katalogkopien werden bytegleich erhalten. Quelladapter und Hostpreflight verweigern IDs außerhalb des Firmware-Importbereichs vor der Paketfreigabe. Die Firmware prüft JPEG-SOI/EOI, baseline SOF0, Abmessungen bis 800 × 800, IDs und den echten LittleFS-Füllstand. Nur produktiv erzeugbare Upload-IDs von 1 bis `0x7fffffff` und Katalogindizes unter 72 sind importierbar; abweichende historische Dateinamen sperren das ganze Paket. Der unveränderliche Firmwarekatalog liegt in einer anderen Partition und wird nicht ersetzt. |
| `littlefs/acme/` | Sieben feste ACME-Dateinamen einschließlich `renewal.json` werden bytegleich erhalten, falls vorhanden. Der gepinnte Client kann `renewal.json` für ARI speichern. Unbekannte Namen und `.katmed-<dezimal>`-Schreibreste sperren die Migration; der physisch bestätigte Reset bereinigt diese Reste über seine bestehende journalisierte Dateistufe. Die ACME-Bibliothek ist im aktuellen Anwendungsfluss nicht aktiviert. |

Die sieben zulässigen regulären Dateien sind einzeln klassifiziert. `account.json` enthält Accountzustand, `directory.json` Verzeichnis-/CA-Metadaten, `account.pem` den privaten Accountschlüssel, `order.json` Bestellzustand, `certkey.pem` den privaten Zertifikatsschlüssel, `certificate.pem` das Zertifikat und `renewal.json` den ARI-/Erneuerungszustand. Auch scheinbar öffentliche JSON-/Zertifikatsdateien gehören als gemeinsam genutzter ACME-Zustand in die vertrauliche Kapsel. Der Import wertet ihre inneren ACME-Formate bewusst nicht aus; er bindet nur feste Pfade, nichtleere Originalbytes, Längen und CRC. Ein künftiger Client-Kompatibilitätsnachweis bleibt erforderlich. Die temporären `.katmed-<dezimal>`-Dateien sind keine kanonischen Nutzdaten und werden weder still überkopiert noch still ausgelassen: ihr Auftreten sperrt den Quelllauf. Der gepinnte Client enthält zusätzlich den transienten Dateipfad `token` für seinen bedingten externen Webserver-/FTP-Zweig. Katmed setzt stattdessen einen lokalen HTTP-01-Server. Ein dennoch vorhandenes `token` sperrt den Export; der bestehende physisch bestätigte Reset kann `token` und dessen Atomic-Reste gezielt beseitigen.

„Optional“ bei `mesh_history` bedeutet ausschließlich, dass dieser Pfad im Inventar fehlen darf. Ist er inventarisiert, muss sein Blob eine der produktiven Mesh-Codec-Längen (2080 oder 2152 Byte) und gültigen Inhalt besitzen. Ein deklarierter leerer oder zu kurzer Blob wird schon beim Host-Export und bei `import-stage` abgewiesen. Der Firmwareparser prüft die Länge nochmals vor Stufe, Journal und Live-Key; ein CRC-konsistentes Capsule macht eine leere History nicht gültig.

Weitere NVS-Namespace-Schlüssel werden **nicht** übernommen. Der tatsächlich aufgerufene `esp_bridge_create_all_netif()`-Pfad der gepinnten IoT Bridge 1.0.1 speichert für die aktivierten Interfaces `WIFI_AP_DEF` und `WIFI_STA_DEF` IP-Info-Blobs sowie die daraus gebildeten U8-Keys `WIFI_AP__check` und `WIFI_STA_check`. Der Quelladapter prüft Namen und NVS-Typen über die vollständige Partition; falsche Typen und fremde Namen sperren den Export. Die Bridge löscht beim Entfernen einer IP-Konfiguration nur den Blob, weshalb der zugehörige Prüfwert allein erhalten bleiben muss. Beschädigte Längen oder U8-Werte außerhalb 0 und 1 sperren Host und Import. Der transiente U8-Key `katmed_reset/prepared`, mögliche alte Wi-Fi-Treiberwerte und unbekannte SDK-Schlüssel sperren weiterhin den Quelllauf. Die priorisierten WLAN-Profile liegen im `katmed/state`-Blob; Bridge-IP-Werte und SDK-Treiberreste sind keine gleichwertigen Profile. Frei editierbare Nutzertexte außerhalb der in diesem Blob enthaltenen Werte sind im aktuellen Store-Schema nicht vorhanden. Einkompilierte private UI-Defaults gehören nicht zu diesem Datenspeichervertrag.

## Ablauf und noch fehlende Geräteschnittstelle

### Lesender produktiver Quelladapter (dieser Folgebaustein)

`firmware/components/katmed_security/katmed_source_export.c` implementiert
`katmed_source_export()` gegen die **produktive** ESP-IDF-NVS-API und das bereits
gemountete `user_images`-LittleFS. Der produktive Aufruf ist nun ausschließlich
im einmaligen Bootfenster über `katmed_source_snapshot_boot()` möglich: nach
Import-Replay, Mount samt Upload-Restbereinigung und Reset-Replay, vor Store-
Öffnung, Mesh, Web, Uploads und ACME. Das Fenster wird vor dem normalen
Dienststart endgültig geschlossen. Der schwache Hook
`katmed_source_snapshot_boot_sink()` liefert standardmäßig `NULL`; erst ein
späterer privater, begrenzter Transport-Sink kann den Snapshot tatsächlich
anfordern. Ein Laufzeitaufruf nach Dienststart wird abgewiesen.

Der nicht wartende Eintritt verweigert aktive Schreiber, offene Bild-Uploads
und aktive ACME-Dateihandles. Store und Mesh-History, Web-Auth und -Identität,
Resetjournal und Reset-Dateistufe, Bildspeicher und der aktive ACME-Client
halten Schreibleases; neue Mutationen werden während der Ausgabe abgewiesen.
Writerzahl und Bootphase liegen in einem gemeinsamen atomaren Wort: Der
Exporter wechselt nur mit exakt null Leases von `open` zu `exporting`, und
ein konkurrierender Writer kann diese Zustandsänderung nicht übersehen.
Gegen fremde und doppelte `leave()`-Aufrufe schützt zusätzlich eine
aufgabeneigene Leasezahl; ein Überlauf wird vor Eintritt abgewiesen.
Ein während des Exports angefordertes Schließen bleibt als ausstehende
Versiegelung erhalten und wird nach der Freigabe endgültig wirksam.
Der gepinnte ACME-Client hält seine Lease bis nach `stop()`, auch wenn seine
internen Dateipfade nicht einzeln kontrolliert werden. NVS wird in fester
Schlüsselreihenfolge, Bild- und ACME-Dateien werden je lexikographisch
ausgegeben. Bei Abbruch, Fehler oder erreichtem 30-Sekunden-Budget wird
`finish(false)` gemeldet und die Sperre freigegeben. Der Sink muss alle
Callbacks einschließlich `keep_going` prompt und begrenzt ausführen;
ein blockierter fremder Callback lässt sich aus demselben Thread nicht sicher
präemptieren. Vor der ersten Ausgabe bindet SHA-256 alle gefundenen NVS- und
Dateibytes. Eine zweite vollständige Leseprüfung bestätigt diesen Bestand
vor Ausgabe. Während der Ausgabe werden NVS-Werte vor ihrem `begin` und
Dateibytes beim Lesen gegen diese Bindung geprüft. Der letzte
`keep_going`-Aufruf liegt **vor** einer abschließenden Inventur und
Leseprüfung, die nur noch interne Deadlineprüfungen ausführt. Zwischen
dieser Prüfung und `finish` werden keine weiteren `keep_going`-, `begin`-,
`bytes`- oder `end`-Callbacks aufgerufen. So werden auch gleichlange
Änderungen aus dem letzten fremden Callback erkannt.
Ein gleichlanger Inhaltswechsel führt zu `finish(false)`. Nur `finish(true)`
nach vollständiger Inventur und Übertragung ist verwendbar; ein Empfänger
muss alle Bytes eines `finish(false)`-Laufs
verwerfen. Der Adapter initialisiert oder formatiert keine Quelle und schreibt
keine privaten Werte in Logs. Ein externer Transport und ein Empfänger, der
daraus geschützte Dateien sowie `source-inventory.json` erstellt, fehlen noch.
Die Garantie setzt die erfassten produktiven Schreibleases und das ruhende
Bootfenster voraus. Ein beliebiger roher paralleler Schreiber, der zwischen
Prüfung und Abschluss mutiert oder seinen alten Inhalt wiederherstellt,
ist damit nicht allgemein kontrolliert; ein Live-Backup ist nicht abgedeckt.

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
| Web-Serveridentität samt privatem Schlüssel und Zertifikat | `katmed_web_identity.c` liest/schreibt `nvs/katmed_web/identity`; der Hostvertrag prüft Form und Länge. Der IDF-Import prüft vor Mutation und Replay das Schlüsselpaar mit der vorhandenen produktiven Validierung. |
| Resetjournal | `katmed_reset_idf.c` liest/schreibt `nvs/katmed_reset/journal`; ein laufender Reset sperrt die Ausgabe. |
| Hochgeladene Nutzerbilder und vorhandene Katalogkopien | `katmed_image_store_idf.c` verwendet `/katmed/user-images`; der Adapter liefert vorhandene `img-XXXXXXXX.jpg` und `cat-XX-[01].jpg` bytegleich. |
| ACME-Zustand, falls die derzeit nicht aktive Bibliothek ihn gespeichert hat | `katmed_acme.cpp` bindet `/katmed/acme` ein; sechs konfigurierte Dateien und das vom gepinnten Client erzeugbare ARI-`renewal.json` sind zugelassen. Atomic-Reste und Fremdnamen sperren vor der Ausgabe. |

Die Quellgrenzen sind höchstens 4096 Byte pro NVS-Blob, 128 KiB pro Datei,
höchstens 128 logische Einträge und weniger als 256 KiB Nutzdateiinhalt mit
8 KiB Reserve. Die zweite Lesepassage begrenzt Datei- und Bytezahl erneut und
verlangt dieselben Summen wie der Vorabscan. Das sind **Vorprüfungen**, keine Garantie, dass ein neuer
24-KiB-NVS-Bereich oder das 256-KiB-LittleFS samt Metadaten passt. Die
Versions- und CRC-Prüfung des Store- und Mesh-Inhalts findet im anschließenden
unveränderten produktiven Host-Codec statt. Bei einer während des Lesens
ändernden Quelle ist kein konsistenter Snapshot zugesichert; deshalb ist das
oben beschriebene Bootfenster mit seinen Schreibleases zwingend.

**Noch nicht angeschlossen beziehungsweise ausdrücklich offen:** Ein
authentisierter und vertraulicher Transport zum vorhandenen Bootaufruf, ein
empfangsseitiges atomisches Anlegen der externen logischen Dateien und
`source-inventory.json`, Kapazitätsnachweise auf echten Partitionen sowie
Geräte- und Recoveryfreigabe. Der begrenzte LittleFS-Importadapter ist vorhanden,
aber ohne produktiven geschützten Empfänger nicht aufrufbar.
Ein SDK-WLAN-NVS-Eintrag oder ein `katmed_acme`-NVS-Eintrag würde derzeit
bewusst sperren; für solche tatsächlich gefundenen Schlüssel muss vor der
Migration eine eigene semantische Zuordnung beschlossen und implementiert
werden. Der Mount bereinigt bekannte abgebrochene `.upload-*.tmp`-Dateien vor
dem Bootfenster; ein danach verbleibender nicht zuordenbarer Quellrest sperrt
den Export. Frei editierbare Texte außerhalb des Store-Blobs wurden in den
aktuellen Persistenzaufrufen nicht gefunden; einkompilierte private UI-Defaults
und flüchtige Sitzungen sind keine gespeicherten Laufzeitdaten.
Der aktuelle ACME-Dateireset, Quelladapter, Hostpaket und Importer erfassen
`renewal.json` gemeinsam. Die innere Semantik des übernommenen ARI-Zustands
ist ohne aktiven Client- und Gerätelauf weiterhin ungeprüft.

### Externes Hostpaket und begrenzter Import

Der Hostaufruf ist `python3 tools/security_migration.py export --source <geschützter-logischer-altbaum> --output <neues-externes-paket>` und danach `python3 tools/security_migration.py import-stage --source <paket> --output <neue-externe-stagingmappe>`. Die Befehle geben nur einen generischen Status aus. Sie dürfen keine echten Daten auf der Konsole oder in CI ausgeben. Ein Fehler lässt den Altbaum und ein bereits vollständiges Paket bestehen; ein neuer Zielname erscheint nur nach vollständigem Schreiben. Die Veröffentlichung nutzt unter Linux `renameat2` mit `RENAME_NOREPLACE`: Wird der Zielname nach der Vorprüfung konkurrierend belegt, schlägt die Aktion fehl und entfernt ihre temporäre Stagingmappe. Ist diese Operation nicht verfügbar, erfolgt keine Veröffentlichung. Ein begrenztes Host-Budget und die 4096-Byte-v11-Codecgrenze führen bei Platzmangel zu Abweisung statt Verkleinerung.

Für einen wirklichen Cutover muss der jetzt aufrufbare ruhende Boot-Exporter an einen begrenzten Transport und einen geschützten, authentisierten Empfänger gebunden werden. Sein Ergebnis muss vor dem Tabellenwechsel extern gesichert und unabhängig gegen diese Paketprüfung verglichen werden. Das bestehende `katmed_nvs_init()` lässt einen belegten unmarkierten NVS-Bereich bewusst nicht als Migration durch. Die im Host erzeugte Stagingmappe ist **keine** geflashte oder gelinkte Runtime-Funktion.

#### Produktiver Teilslice

`import-stage` schreibt zusätzlich `firmware-import-v1.bin` in die geschützte externe Stagingmappe. Dessen `KMI1`-Header bindet Version 1, Gesamtlänge, Eintragszahl und CRC32 aller folgenden Records; jeder Record trägt Pfadlänge, Datenlänge, CRC32 und die logischen Pfad-/Datenbytes. Die Hostseite erstellt ihn **erst nach** Inventur, SHA-256-Manifestprüfung und Prüfung der produktiven Store-/Mesh-Codecs. Der Firmwarekern in `katmed_import.cpp` prüft den gesamten Capsuleinhalt noch einmal, bevor er einen Zielschlüssel schreibt: Format, Integrität, bekannte und eindeutige Pfade, Längen, höchstens 128 Einträge, höchstens 248 KiB gemeinsame LittleFS-Nutzlast, JPEG-Grundform und zulässige Bild-/Katalog-IDs, die sieben festen ACME-Namen und nichtleere ACME-Bytes, die vier festen Bridge-NVS-Pfade mit ihren Blob- beziehungsweise U8-Längen sowie ein kanonisches v11-Store-Blob einschließlich Sprachbyte und optional die durch den produktiven Mesh-Codec geprüfte `mesh_history`. Das vollständige Capsule ist auf 320 KiB begrenzt; es wird aus dem späteren Empfänger gelesen und nicht als Ganzes im Ziel-NVS oder LittleFS dupliziert. Ein direktes v10-Capsule wird vor Mutation abgewiesen; die semantische v10→v11-Umwandlung gehört in den geprüften Hostpaketpfad. CRC32 erkennt Beschädigung; weder CRC noch Paket-SHA authentisieren einen Absender.

Das Capsule dupliziert die logischen Werte innerhalb der externen, geschützten Stagingmappe. Der voreingestellte Wert von `--max-stage-bytes` ist 264 KiB, also 248 KiB Quell-Dateiinhalt plus höchstens 16 KiB NVS-Werte. Er begrenzt die Summe der logischen Zielwerte und ist keine Zusage über den gesamten externen Plattenverbrauch. Ein Schreib-/Platzfehler verhindert die atomare Veröffentlichung der Stagingmappe.

Bei Web-Werten prüft die Firmware vor der ersten NVS-Mutation und bei jedem Replay außerdem das echte `KatmedWebAuthRecord`- und `KatmedWebIdentity`-Layout. Der IDF-Backendcallback verwendet den produktiven X.509-Parser, den gespeicherten Client-Zertifikatfingerprint und `identity_valid()` für das Server-Schlüsselpaar; ein synthetischer Host-Backendcallback ersetzt diese Geräteprüfung nicht. Für die vollständige NVS-Kombination werden Stufe und Zielwerte gemeinsam gegen `nvs_stats_t.available_entries` vorgeprüft. Dieser IDF-Wert zieht die für NVS-Garbage-Collection reservierte 126-Entry-Seite bereits von `free_entries` ab; der Importkern fordert darüber hinaus seine eigene 24-Entry-Reserve. Jeder Commit wird zurückgelesen; Schreib-, Platz- oder Rücklesefehler lassen das Journal für Replay stehen oder brechen vor dessen Veröffentlichung ohne Live-Key ab. Die Entry-Vorprüfung belegt keinen tatsächlichen Flash-Freiraum auf einem Gerät.

**Aktuell übernehmbar sind `nvs/katmed/state`, optionale `mesh_history`, optionale `katmed_web/identity`, `katmed_web/auth` nur zusammen mit der Identität, ein optionales ruhendes `katmed_reset/journal`, bis zu vier bekannte Bridge-IP-/U8-Einträge, gültige Nutzerbilder und die sieben festen ACME-Dateien.** Deklarierte leere oder beschädigte Werte werden nicht als abwesend ausgegeben. Das Resetjournal ist keine Resetfreigabe: `pending=1` sperrt, und ein übernommenes `pending=0` startet keine Löschaktion. Stufen ohne Bridge-Einträge bleiben `KMS2` oder, mit Dateien, `KMS3`; Bridge-Capsules verwenden `KMS4` mit neun NVS-Längen und optionalen Dateideskriptoren. Prüfwerte werden im Ziel als NVS-U8 geschrieben und zurückgelesen. Art 3 bildet IDs 0 bis 6 exakt auf die oben genannten ACME-Namen ab. Ältere Store-/Mesh-Stufen bleiben für Replay lesbar. Unbekannte Pfade werden vor der ersten Zielmutation abgewiesen.

`katmed_import_apply_idf()` ist ein bewusst noch unverdrahteter Produktaufruf für einen späteren geschützten Empfänger. Der Aufrufer muss nach gesicherter NVS-Initialisierung ein stabiles, vollständiges Capsule und die lokal gelesene Device-ID bereitstellen und alle Schreiber anhalten. Der IDF-Adapter vergleicht diese ID erneut mit der read-only Factory-MAC; eine fremde Device-ID im Store-Blob sperrt vor Mutation. Im neutralen Profil verweigert er jeden Import. Im Security-Profil prüft er das leere NVS-Inventar, tatsächlich verfügbare NVS-Entries und das passende leere `user_images`-LittleFS. Der Dateiadapter mountet ohne Formatierung und fordert anhand des tatsächlichen `esp_littlefs_info()`-Füllstands mindestens alle Bildbytes plus 32 KiB Grundreserve und 256 Byte je Datei. Die Prüfung ist eine konservative Ablehnungsgrenze; reale COW-Metadaten können später trotzdem einen Schreibfehler auslösen. Nach dauerhaftem Schreiben und Rücklesen der NVS-Stufe legt er alle Dateien unter nichtöffentlichen `.migration-*`-Namen ab, synchronisiert sie und liest Länge und CRC zurück. Erst danach wird das CRC-gebundene NVS-Journal veröffentlicht. Replay prüft die Stufe und lokale Factory-MAC erneut, benennt jede passende temporäre Datei auf ihren endgültigen Namen um und prüft alle finalen Dateien vor dem ersten Live-NVS-Key. Bereits korrekt veröffentlichte Dateien werden bei Wiederholung nur geprüft; abweichende vorhandene Dateien werden nie überschrieben. Danach übernimmt er jeden deklarierten NVS-Key und liest ihn zurück. Erst nach allen Übereinstimmungen entfernt er Journal und Stufe. Ein vor Journalbildung verwaister Stufenwert hält den Boot an und kann nur mit `katmed_import_abort()` unter Abwesenheit aller Live-Werte verworfen werden. Der Secure-Bootpfad ruft Replay nach `katmed_nvs_init()` und vor Diensten auf; fehlende, beschädigte oder unvollständige FS-Dateien halten den normalen Boot. NVS-Commit, LittleFS-Sync/Rename und Rücklesen entscheiden tatsächlich über Erfolg; weder die Entry-Zahl noch der Host-Paketrahmen beweisen Gerätekapazität oder Stromausfallverhalten.

Der Abort eines vorjournalisierten Orphans verlangt zusätzlich eine vollständige kanonische Stufenprüfung gegen die lokal gelesene Device-ID. Der IDF-Wrapper liest dafür die Factory-MAC erneut. Fremde oder beschädigte Stufen und ihre Tempdateien bleiben unverändert für eine getrennte Recoveryentscheidung; auch der automatische Abort nach einem Apply-Fehler verwendet diese Bindung.

**Noch offen:** Transport und geschützter authentisierter Empfänger für das Capsule, Quieszenz für einen möglichen Export nach Dienststart und auf der späteren Zielseite sowie ein Nachweis, dass der spätere ACME-Client die bytegleich übernommenen inneren Zustände akzeptiert. Der bestehende Factory-Reset erreicht ACME-Dateien ausschließlich über seine physische Freigabe und journalisierte Dateistufe; der Import löst keinen Reset aus. Die Bilddateien sind an Dateinamen, IDs, Längen und CRC gebunden; die im Store gespeicherte Katalogkennung beschreibt dagegen den unveränderlichen Firmwarekatalog und wird durch diesen Dateislice nicht neu gebunden. Auf einem echten Gerät fehlen eine Füllstands-/Stromausfallprobe und ein Nachweis gegen konkurrierende Schreiber. Ohne diese Anschlüsse und gerätegebundene Recoveryprobe ist der Adapter keine Migrations- oder Sicherheitsfreigabe.

Die aktuellen Standardlayouts verwenden ein gemeinsames encrypted LittleFS ab
`0x280000` bis zum jeweiligen Flashende (C3 4 MiB, C6 8 MiB). Die Securitylayouts
verwenden denselben gemeinsamen Dateibestand ab `0x236000`; C3 umfasst
`0x1ca000`, C6 `0x5ca000` Byte. Eine separate Stockpartition existiert nicht mehr.
Standarddateien liegen geschützt in `factory-images`, Uploads in `user-images`.
Der Import und Factory Reset formatieren das Dateisystem niemals; Standarddateien
werden nicht als Benutzerdaten exportiert oder überschrieben. Der vorhandene
begrenzte semantische Migrationsvertrag wird dadurch nicht erweitert.


Historischer Versuchsstand vor der Zusammenlegung des Bilderdateisystems: Die damalige Erweiterung auf 576 KiB war durch echte gepinnte LittleFS-Blockengineproben motiviert: Ein synthetischer, JPEG-decodierbarer KMI1-Bestand mit 127 Bildern und exakt 248 KiB Inhalt passte wegen der 4-KiB-Blockbelegung weder in 256 noch in 512 KiB, ließ sich aber auf 576 KiB vollständig einspielen, nach Remount anhand jeder Dateilänge und CRC prüfen und nach unterbrochenem Publish wiederaufnehmen. Zusätzlich wurde ein physisch in 256 KiB passender Altbestand aus vier Bildern mit zusammen 220 KiB auf der echten Blockengine angelegt, remountet und auf das größere Ziel migriert. Diese Proben belegen konkrete volle Fälle, nicht die Kapazität jedes möglichen Dateilängenmusters oder das Verhalten eines echten Geräts. Der aktuelle IDF-Adapter verlangt das targetabhängig exakt `0x1ca000` Byte (C3) beziehungsweise `0x5ca000` Byte (C6) große gemountete Security-Ziel und prüft dessen tatsächlichen LittleFS-Füllstand plus 32 KiB Grundreserve und 256 Byte je Datei vor dem ersten Zielwrite; nach jedem Tempfile verlangt er mindestens 8 KiB freie Laufreserve. Schreib-, Sync- und Rücklesefehler führen nie zu einem als fertig gemeldeten Teilbestand. Die Security-NVS-Partition wächst auf sieben 4-KiB-Seiten beziehungsweise 28 KiB. Ihre 882 physischen Entries ergeben nach der vom IDF reservierten 126-Entry-Seite höchstens 756 nutzbare Entries. Eine gültige Kombination aus 123 Bildern und allen fünf maximalen NVS-Werten benötigt nach der konservativen Kernformel 617 Entries plus 24 eigene Reserve, also 641; der 97-Bilder-Randfall benötigt 631 und scheitert auf dem alten Sechs-Seiten-Ziel mit höchstens 630 nutzbaren Entries. Der produktive Callback liefert nun `available_entries` statt `free_entries`. Die später tatsächlich freien Entries werden unmittelbar vor Stage, Journal und Live-Werten vorgeprüft; Schreib- und Rücklesefehler bleiben maßgeblich. Der kleinere App-Slot und die Laufzeit-Heapgrenze brauchen vor Transport, Update und Gerätefreigabe neue Nachweise.

`katmed_import_abort_idf()` stellt den begrenzten Abbruch für eine Stufe ohne Journal und ohne produktive Zielkeys bereit. Ein ausstehendes Journal lässt sich bewusst nicht abbrechen; es muss aus der geprüften Stufe wiederaufgenommen werden. Für den Abbruch existiert noch keine Nutzeraktion oder Transportsteuerung.

Das Layout verlegt Teile von NVS und Schlüsselbereich in die frühere Factory-App. Weder ein rohes Alt-NVS-Image noch eine alte Partitionstabelle oder ein gewöhnlicher Klartext-Flash sind daher ein zulässiger Import. Der spätere Release-Mode-Update-/Recoveryweg, individuelle Schlüssel, die allgemeine NVS-Integritätsgrenze und echte Geräteabnahme bleiben gesondert offen. Der veröffentlichte ACME-Dateireset einschließlich `renewal.json` bleibt erhalten; Source-, Host- und Importvertrag teilen nun dieselben sieben regulären Namen. Der Gerätebereitschaftsstatus lautet „Noch nicht bereit“.

Primärquellen: [ESP-IDF 6.1 NVS Encryption](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/storage/nvs_encryption.html), [NVS-API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/storage/nvs_flash.html) und [Flash Encryption/Update-Grenzen](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/security/flash-encryption.html). Die Hostprüfung bindet außerdem die unveränderten Quellcodecs `katmed_store.cpp` und `katmed_mesh_status.cpp` ein.


## Begrenzter Layoutvorschlag für neue Mesh-Namen vom 22. Oktober 2026

Dieser Abschnitt beschreibt einen noch unabhängig zu prüfenden Sourcevorschlag.
Er ersetzt für diesen Vorschlag die oben historischen Security-Grenzen, ohne den
alten Migrationsumfang erneut zu bestellen. Alte Gerätedatenübernahme ist nicht
beauftragt. Die normalen neuen Namensdaten bleiben dauerhaft zu erhalten.

Beide Security-Tabellen lassen Factory bei `0x20000` beginnen und vergrößern
sie um 24 KiB auf `0x216000` Byte. Das gemeinsame Security-Dateisystem beginnt bei `0x236000` und reicht bis zum
Flashende. Bootloader-, Tabellen-, NVS- und Appadressen bleiben unverändert.
Der alte zusätzliche Bildpack entfällt; relativer Dateizugriff und Seed ersetzen
seinen Paketladepfad. Vorhandene historische Captures bleiben ihrer damaligen
Geometrie zugeordnet und sind kein aktueller Installationssatz.


Alle geänderten Datengrenzen sind 4096-Byte-ausgerichtet; die unveränderte
Appadresse ist 65536-Byte-ausgerichtet. NVS, Schlüssel, Guard, PHY,
Partitionstisch und Bootloaderadressen bleiben gleich. Der Runtimecheck muss
denselben neuen FS-Beginn verlangen. Verschlüsselung verwendet die tatsächliche
Partitionadresse; bereits verschlüsselte alte FS-Bytes dürfen nicht an eine
andere Adresse kopiert werden. Der labelbasierte Bildreader verwendet die
neue Tabellenadresse und die gemeinsame Dateisystemgröße.

Das ist kein in-place Firmwareupdate für belegte alte Geräte. Eine alte
Partitionstabelle mit neuer App wird durch den Runtimecheck abgewiesen. Ein
neuer Tisch mit alten FS-Daten führt zum Mountfehler; automatische Formatierung
bleibt ausgeschaltet. Ein alter Bildpack ist kein gültiges neues Dateisystem. Eine spätere ausdrücklich
autorisierte Neuinstallation oder Recovery muss zusammengehörige App, Tabelle
und ein passend aufgebautes gemeinsames FS-Seedimage verwenden und den bestehenden
Verschlüsselungs-/Schlüsselvertrag beachten. Hier wird weder ein Flashbefehl
noch ein neuer Import-, Migration-, OTA- oder Recoverydienst eingeführt.

`littlefs_create_partition_image(user_images, ...)` liest Größe und Adresse
aus dem Partitionstisch und registriert einen separaten verschlüsselungsbewussten
Flash-Target. Ohne `FLASH_IN_PROJECT` gehört dieses Image weiterhin nicht zum
allgemeinen Flashlauf. Das gemeinsame Bilderdateisystem ist der einzige Bildbestand des neuen Satzes.
Vorhandene historische Provisionierungs-Captures mit `0x2c0000` gehören zu ihrem
alten Layout und sind keine ausführbaren Recoveryanweisungen für diesen Vorschlag.
Der Security-Buildwrapper erlaubt weiterhin nur Bauen. Gerätebereitschaft fehlt;
keine Geräte- oder Flashaktion ist durch dieses Dokument freigegeben.
