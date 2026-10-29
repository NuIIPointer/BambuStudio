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
3. Abschnitte mit Stufe ≠ 0 werden aus der Wandschleife herausgeschnitten. Sie kommen als eigene Insel-Collection an den Anfang von `LayerRegion::perimeters`, die Stufe steht parallel in `LayerRegion::perimeters_shade`. Die Außenwand bleibt unberührt.

## Planung pro Schicht (`plan_shade`)

`d = |Stufe| / 16 × shading_strength`, Filament = Hell- bzw. Dunkel-Filament.

- **Modus „Schichtwechsel“** (Standard für AMS): Ganze Schichten der Innenwand werden in der Schattierfarbe gedruckt, gleichmäßig verteilt über Z. Schicht i ist „an“, wenn `round((i+1)·d) > round(i·d)`.
- **Modus „Schichtdicke“** (Teilschichten, Umsetzung des Schichtdicken-Konzepts): Die Innenwand einer Schicht wird in zwei Teilschichten geteilt, Schattierfarbe mit Höhe `h_s` und Hauptfarbe mit Höhe `lh − h_s`. `h_s` folgt einer Fehlerdiffusion über Z: `Q(d·z_i) − Q(d·z_{i−1})` mit `Q(x) = round(x/h_min)·h_min` (`h_min` = `shading_min_sublayer_height`, Standard 0.04 mm). Ist ein Rest kleiner als `h_min/2`, entfällt er. Die Schattierfarbe bekommt höchstens `lh − h_min`, die Hauptfarbe behält also immer eine obere Teilschicht (siehe Ober- und Unterseiten). So bekommt man feinere Abstufungen als mit ganzen Schichten, ohne zu dünne Teilschichten.
  - Welche Teilschicht unten liegt, ergibt sich aus der Reihenfolge der Filamente in der Schicht: Die zuerst gedruckte liegt unten. So muss die Werkzeugreihenfolge (Spülmengen-Optimierung) nicht geändert werden. Schatten-Collections stehen vorne in der Wandliste, so wird die untere Teilschicht vor der Außenwand gedruckt, und die Düse fährt nie neben einer schon fertigen Außenwand nach unten.
- **Außenwand-Tönung** (optional, `shading_outer_wall_ratio`, Standard 0 = aus): nur im Modus „Schichtdicke“. Bei starker Schattierung wird zusätzlich eine untere Teilschicht der Außenwand in der Schattierfarbe gedruckt (Höhe `ratio × d × lh`, mindestens `h_min`). Das ist die Schichtdicken-Idee für die Außenwand und gibt deutlich kräftigere Effekte, die Oberfläche zeigt dann aber feine Streifen.
- Die erste Schicht wird nie schattiert.

`ToolOrdering::collect_extruders` nimmt die Schattierfilamente pro Schicht auf, wenn `plan_shade` sie braucht. Dadurch arbeiten Wischturm, Spülung, Filament-Gruppierung (H2C) und AMS-Wechsel ohne Sonderfälle. Zusätzlich gelten Schattierfilamente in `PrintObject`/`Print` als genutzte Filamente, damit die Düsenzuordnung beim H2C sie kennt.

`GCode::process_layer` ordnet Schatten-Collections dem Filament aus `plan_shade` zu. Bei Teilschichten wird dieselbe Collection beiden Filamenten zugeordnet. Höhe, Z und Fluss werden über die vorhandene Teilschicht-Mechanik (`m_sub_layer_flow_ratio`, `m_sub_layer_height`, `m_nominal_z`) pro Collection gesetzt.

## Einstellungen (Prozess → Qualität → Airbrush-Shading)

| Schlüssel | Typ | Standard | Bedeutung |
|---|---|---|---|
| `shading_light_filament` | int, Filament-Auswahl | 0 | Filament für helleres Shading, „Standard“ (0) = automatisch (hellstes Filament) |
| `shading_dark_filament` | int, Filament-Auswahl | 0 | Filament für dunkleres Shading, „Standard“ (0) = automatisch (dunkelstes Filament) |
| `shading_mode` | enum | `layer_alternation` | `layer_alternation` oder `sublayer` |
| `shading_wall_depth` | int | 1 | Anzahl Innenwände hinter der Außenwand, die getönt werden |
| `shading_strength` | percent | 100 % | skaliert alle Stufen |
| `shading_min_sublayer_height` | float | 0.04 | kleinste Teilschicht |
| `shading_outer_wall_ratio` | percent | 0 % | Außenwand-Tönung, nur im Modus `sublayer` |

Die beiden Filamente gelten für den ganzen Druck (`PrintConfig`) und werden wie das Support-Filament aus der Filamentliste gewählt. Alle anderen Einstellungen stehen in `PrintObjectConfig` und lassen sich pro Objekt ändern (Objekt-Ansicht der Prozesseinstellungen und Objektliste → Qualität).

Die automatische Wahl nimmt das physische Filament (kein Misch-Slot) mit der höchsten bzw. niedrigsten Luminanz, aber nur ab 0,7 (hell) bzw. bis 0,2 (dunkel). Ein kräftiges Rot (etwa 0,3) zählt also nicht als dunkel. Ist das gewählte Filament gleich der Hauptfarbe, entfällt die Schattierung an dieser Stelle.

## Ober- und Unterseiten

Bemalte Flächen mit |n_z| > 0.9 wirken auf die massive Füllung direkt unter der Oberseite bzw. direkt über der Unterseite. Die oberste und die unterste Schicht bleiben in der Hauptfarbe, wie die Außenwand.

1. Nach dem Füllungsschritt (`posInfill`) baut ein **SkinShadeField** die XY-Projektion der bemalten nach oben bzw. unten zeigenden Dreiecke pro Stufe auf.
2. Für Schicht i: Oberseitenfläche der Schicht i+1 (`lslices(i+1) − lslices(i+2)`) geschnitten mit den nach oben zeigenden Flächen in deren Höhe, genauso die Unterseitenfläche der Schicht i−1 mit den nach unten zeigenden. Das Ergebnis wird 0,6 mm von den Wänden ferngehalten, damit die Düse einer unteren Teilschicht die Wand nicht berührt.
3. Die Bahnen der massiven Füllung (`erSolidInfill`) werden an diesen Flächen zerschnitten. Stücke unter 0,6 mm bleiben in der Hauptfarbe. Die Schattenstücke kommen als eigene Collections in `LayerRegion::fills`, die Stufe steht in `LayerRegion::fills_shade`. Später angehängte Füllungen (Bügeln) haben keine Markierung.
4. `plan_skin_shade`: Eine Fläche ist nur eine Schicht dick, deshalb entsteht der Ton immer über eine Teilschicht, in beiden Modi: Höhe `round(d·lh / h_min)·h_min`, höchstens `lh − h_min`.

Die Ausgabe nutzt dieselben Teilschichten wie die Wände (zwei Durchgänge, untere Teile zuerst). Dass die Schattierfarbe nie die volle Höhe druckt, verhindert, dass ein zweites Schattierfilament später in derselben Schicht neben einem schon fertigen Stück tiefer druckt. Das kam vorher an Wandecken und im Schachbrett vor.

Grenzen: Mit nur einer oberen bzw. unteren massiven Schicht gibt es keine Schicht dazwischen, dann bleibt die Fläche ohne Ton. Wie stark eine Schicht in der Hauptfarbe den Ton durchscheinen lässt, hängt vom Filament ab und ist noch nicht am Druck geprüft.

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

Nachtrag 2026-10-29: Einstellungen pro Objekt, Filament-Auswahl, Ober- und Unterseiten. Testdatei mit zwei 30-mm-Würfeln (je einer pro Modus, Modus pro Objekt gesetzt), jede Seite mit eigenem Muster: Höhenverlauf, Verlauf entlang der Wand, Schachbrett und Streifen 4 bis 1 mm, Tonstufen und weiche Punkte, oben Schachbrett und Verlauf, unten Verlauf. Ausgewertet aus dem G-Code:
- Alle Muster erscheinen auf der Innenwand an der richtigen Stelle. Streifen unter etwa 2 mm verschwimmen durch die Mittelung entlang der Wand zu einem Mittelton.
- Unter der Oberseite und über der Unterseite erscheinen Schachbrett und Verläufe in der massiven Füllung, in beiden Würfeln.
- Keine Extrusion unterhalb der Schichtoberkante näher als 0,4 mm an einer schon in voller Höhe gedruckten Bahn derselben Schicht (vorher 67 Stellen).
- Druckzeit auf dem H2C rund 2 h 45 min für beide Würfel.

Bekannte Grenzen:
- Druckzeit: Jede schattierte Schicht braucht Filamentwechsel. Der Testzylinder (Ø30 × 30 mm) braucht auf dem H2C rund 1 h 10 min statt 19 min.
- H2C über die Kommandozeile: Ohne Drucker-Synchronisation fehlen Düsenbestand und Filament-Zuordnung im Projekt. Für den Test wurden sie von Hand gesetzt.
- Die Bedienung des Gizmos und die neuen Filament-Auswahlfelder sind kompiliert, aber nicht von Hand in der Oberfläche getestet.
- Im Modus Teilschicht-Dicke erreicht ein voll gesprühter Ton 80 % statt 100 % der Schichthöhe (bei 0,2 mm Schicht und 0,04 mm Mindest-Teilschicht).
