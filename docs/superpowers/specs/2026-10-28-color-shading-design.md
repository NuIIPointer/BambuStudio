# Color Shading (Airbrush) – Design

Stand 2026-10-28. Branch `feature/color-shading`, Basis: Bambu Studio `master` 79df7ae (2.8.4), alter Fork-Stand wird nicht übernommen.

## Ziel

Das vorhandene Color Mixing bekommt eine Schattierungsfunktion. Die Außenwand wird immer in der Hauptfarbe gedruckt. Die Innenwand direkt dahinter bleibt entweder unverändert oder wird in Weiß bzw. Schwarz gedruckt. Weil die Außenwand dünn ist, schimmert die Innenwand durch, so entsteht helleres oder dunkleres Shading. Wie oft Weiß bzw. Schwarz statt der Hauptfarbe gedruckt wird, ergibt den Verlauf.

Festgelegt hat der Nutzer:
- Ein neues Mal-Werkzeug **Airbrush**, mit dem man Verläufe „sprüht“: „dunkler sprühen“ und „heller sprühen“. Das Werkzeug ist ausschließlich für Schattierung da.
- Die Idee „Schichtdicke für Farbwechsel“ soll mit einfließen, für Innen- und Außenwand.
- Getestet wird auf dem H2C, es muss aber auch mit AMS-Druckern (eine Düse) funktionieren.

Nicht-Ziele: Das Airbrush verändert nicht die Farbe der Außenwand (Ausnahme: optionale Außenwand-Tönung, siehe unten). Es gibt keine automatische Schattierung aus der Geometrie.

## Datenmodell

- Neue Annotation `ModelVolume::shade_facets` (`FacetsAnnotation`), genau wie `fuzzy_skin_facets`.
- Zustände: 0 = unverändert, 1–16 = dunkler (Stufe 1/16 … 16/16), 17–32 = heller (Stufe 1/16 … 16/16). Damit reicht der vorhandene Wertebereich bis `ExtruderMax` (32), und Serialisierung, Undo/Redo und Rendering pro Zustand funktionieren ohne Formatänderung.
- 3MF-Attribut `paint_shade` pro Dreieck. Ältere Versionen ignorieren es.
- Helfer in `libslic3r/Shading.hpp`: Umrechnung Zustand ↔ Stufe (−16…+16), Zuordnung der Schattierfilamente und Schichtplanung.

## Airbrush-Werkzeug (GUI)

- Neuer Gizmo `GLGizmoAirbrush` nach dem Muster von `GLGizmoFuzzySkin`. Er hat einen eigenen Eintrag in der linken Werkzeugleiste und das Tastenkürzel `Ctrl+J`.
- Werkzeuge:
  - **Airbrush:** Kreis oder Kugel. Jede Maus-Bewegung ist ein „Sprühstoß“. Er erhöht bzw. verringert die Stufe der Dreiecke unter dem Pinsel mit weichem Rand (Cosinus-Abfall). Einstellbar sind Stärke (Stufen pro Stoß) und Härte (Anteil des Pinsels ohne Abfall). Die Zufallsrundung der Stärke gibt den Sprüh-Look, wiederholtes Sprühen baut Verläufe auf. Linke Maustaste = gewählte Richtung, rechte Maustaste = Gegenrichtung, Shift = radieren.
  - **Füllen / Smart Fill:** setzt eine feste Stufe auf zusammenhängende Flächen.
  - **Verlauf:** Zieht man eine Linie über die Fläche, bekommt die angeklickte Fläche (Smart-Fill-Region) einen linearen Verlauf von Start- zu Endstufe entlang der Linie.
  - „Alles löschen“.
- Anzeige: Jede Stufe wird als Mischung aus der Filamentfarbe des Teils und Schwarz bzw. Weiß dargestellt.
- Neue Triangle-Selector-Funktion `airbrush_patch()`: Sie teilt die Dreiecke im Pinsel fein genug für den Abfall und wendet pro Blatt-Dreieck eine Zustandsfunktion mit Gewicht an.

## Slicing

Schritt `posPerimeters` (Änderungen am Airbrush invalidieren nur Wände und G-Code, nicht das Slicing):

1. Pro Objekt wird ein **ShadeField** aufgebaut: alle bemalten und unbemalten Seitenflächen der Modellteile im Objekt-Koordinatensystem, mit Stufe pro Dreieck, in einem AABB-Baum. Fast waagrechte Flächen (|n_z| > 0.9) werden ausgelassen, damit Deckel und Boden die Seitenbemalung nicht überdecken.
2. Nach dem Wandgenerator (klassisch und Arachne) werden alle **inneren** Wandpfade (`erPerimeter`) abgetastet, Schritt 0.3 mm. Liegt die nächste Seitenfläche näher als `Breite_Außenwand + Tiefe × Breite_Innenwand`, übernimmt der Punkt deren Stufe. Läufe mit gleicher Stufe unter 1 mm Länge werden mit den Nachbarn verschmolzen.
3. Abschnitte mit Stufe ≠ 0 werden aus der Wandschleife herausgeschnitten. Sie kommen als eigene Insel-Collection an den Anfang von `LayerRegion::perimeters`, die Stufe steht parallel in `LayerRegion::perimeters_shade_level`. Die Außenwand bleibt unberührt.

## Planung pro Schicht (`plan_shade`)

`d = |Stufe| / 16 × shading_strength`, Filament = Hell- bzw. Dunkel-Filament.

- **Modus „Schichtwechsel“** (Standard für AMS): Ganze Schichten der Innenwand werden in der Schattierfarbe gedruckt, gleichmäßig verteilt über Z. Schicht i ist „an“, wenn `round((i+1)·d) > round(i·d)`.
- **Modus „Schichtdicke“** (Teilschichten, Umsetzung des Schichtdicken-Konzepts): Die Innenwand einer Schicht wird in zwei Teilschichten geteilt, Schattierfarbe mit Höhe `h_s` und Hauptfarbe mit Höhe `lh − h_s`. `h_s` folgt einer Fehlerdiffusion über Z: `Q(d·z_i) − Q(d·z_{i−1})` mit `Q(x) = round(x/h_min)·h_min` (`h_min` = `shading_min_sublayer_height`, Standard 0.04 mm). Ist ein Rest kleiner als `h_min/2`, wird er zur vollen Schicht bzw. zu 0. So bekommt man feinere Abstufungen als mit ganzen Schichten, ohne zu dünne Teilschichten.
  - Welche Teilschicht unten liegt, ergibt sich aus der Reihenfolge der Filamente in der Schicht: Die zuerst gedruckte liegt unten. So muss die Werkzeugreihenfolge (Spülmengen-Optimierung) nicht geändert werden. Schatten-Collections stehen vorne in der Wandliste, so wird die untere Teilschicht vor der Außenwand gedruckt, und die Düse fährt nie neben einer schon fertigen Außenwand nach unten.
- **Außenwand-Tönung** (optional, `shading_outer_wall_ratio`, Standard 0 = aus): nur im Modus „Schichtdicke“. Bei starker Schattierung wird zusätzlich eine untere Teilschicht der Außenwand in der Schattierfarbe gedruckt (Höhe `ratio × d × lh`, mindestens `h_min`). Das ist die Schichtdicken-Idee für die Außenwand und gibt deutlich kräftigere Effekte, die Oberfläche zeigt dann aber feine Streifen.
- Die erste Schicht wird nie schattiert.

`ToolOrdering::collect_extruders` nimmt die Schattierfilamente pro Schicht auf, wenn `plan_shade` sie braucht. Dadurch arbeiten Wischturm, Spülung, Filament-Gruppierung (H2C) und AMS-Wechsel ohne Sonderfälle. Zusätzlich gelten Schattierfilamente in `PrintObject`/`Print` als genutzte Filamente, damit die Düsenzuordnung beim H2C sie kennt.

`GCode::process_layer` ordnet Schatten-Collections dem Filament aus `plan_shade` zu. Bei Teilschichten wird dieselbe Collection beiden Filamenten zugeordnet. Höhe, Z und Fluss werden über die vorhandene Teilschicht-Mechanik (`m_sub_layer_flow_ratio`, `m_sub_layer_height`, `m_nominal_z`) pro Collection gesetzt.

## Einstellungen (Prozess → Sonstiges → Schattierung)

| Schlüssel | Typ | Standard | Bedeutung |
|---|---|---|---|
| `shading_light_filament` | int | 0 | Filament für helleres Shading, 0 = automatisch (hellstes Filament) |
| `shading_dark_filament` | int | 0 | Filament für dunkleres Shading, 0 = automatisch (dunkelstes Filament) |
| `shading_mode` | enum | `layer_alternation` | `layer_alternation` oder `sublayer` |
| `shading_wall_depth` | int | 1 | Anzahl Innenwände hinter der Außenwand, die getönt werden |
| `shading_strength` | percent | 100 % | skaliert alle Stufen |
| `shading_min_sublayer_height` | float | 0.04 | kleinste Teilschicht |
| `shading_outer_wall_ratio` | percent | 0 % | Außenwand-Tönung, nur im Modus `sublayer` |

Die automatische Wahl nimmt das physische Filament (kein Misch-Slot) mit der höchsten bzw. niedrigsten Luminanz. Ist das gewählte Filament gleich der Hauptfarbe, entfällt die Schattierung an dieser Stelle.

## Grenzen und Risiken

- Mit AMS bzw. einer Düse bedeutet jede schattierte Schicht bis zu zwei zusätzliche Filamentwechsel mit Spülung. Die Validierung gibt einen Hinweis aus. Empfohlen wird „Spülen in Infill“.
- Spiralvase wird zusammen mit Schattierung abgelehnt.
- Ist die Hauptfarbe ein Misch-Slot mit Teilschichten, wird im Modus `sublayer` auf `layer_alternation` zurückgefallen.

## Stand der Umsetzung (2026-10-29)

Umgesetzt wie oben beschrieben: Datenmodell und 3MF (`paint_shade`), Airbrush-Gizmo (Kreis/Kugel, Füllen, Verlauf; `Ctrl+J`), Wand-Aufteilung, beide Modi, Außenwand-Tönung, Werkzeugreihenfolge, genutzte Filamente, deutsche Übersetzung.

Abweichungen vom Entwurf, als eigene Entscheidung:
- Die Stufen werden entlang der Wand über 2 mm gemittelt, und Abschnitte unter 2 mm werden mit dem Nachbarn mit der ähnlicheren Stufe zusammengelegt. Grund: Nebeneinanderliegende Dreiecke unterscheiden sich oft um eine Stufe, und ein Sprühstoß ist absichtlich körnig. Ohne Mittelung würde die Wand alle paar Zehntelmillimeter das Filament wechseln.
- Beim Erzeugen der Wände wird die Teilschicht-Reihenfolge nicht festgelegt. Beim G-Code druckt jedes Filament zuerst die Teile, bei denen es unten liegt, danach den Rest.
- Die Bemalung wird bei „Mesh reparieren mit Bemalung erhalten“ noch nicht neu projiziert, sondern verworfen.

Getestet über die Kommandozeile mit einem bemalten Zylinder: links ein Verlauf von Hell nach unverändert, rechts von unverändert nach Dunkel. Gemessen wurde der Materialanteil des Shading-Filaments an der Innenwand pro Höhenband.
- X1C (AMS, eine Düse) und H2C (Düsenwechsler), jeweils in beiden Modi: Der Anteil folgt dem gemalten Verlauf. Im Modus Teilschicht-Dicke ist er nahezu linear (Weiß 91 % → 5 %, Schwarz 4 % → 91 %).
- Die Außenwand bleibt ohne Tönung immer in der Hauptfarbe.
- Teilschicht-Modus: Nach einer Außenwand in voller Höhe wird in derselben Schicht nichts tiefer gedruckt.
- 3MF speichern und laden: Die Bemalung bleibt bit-genau erhalten.

Bekannte Grenzen:
- Druckzeit: Jede schattierte Schicht braucht Filamentwechsel. Der Testzylinder (Ø30 × 30 mm) braucht auf dem H2C rund 1 h 10 min statt 19 min.
- H2C über die Kommandozeile: Ohne Drucker-Synchronisation fehlen Düsenbestand und Filament-Zuordnung im Projekt. Für den Test wurden sie von Hand gesetzt.
- Die Bedienung des Gizmos ist kompiliert, aber nicht von Hand in der Oberfläche getestet.
