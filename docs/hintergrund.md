---
title: Hintergrund — wie ProjektX funktioniert
layout: default
---

# Hintergrund — wie ProjektX funktioniert

*Diese Seite erklärt, warum sich ProjektX so verhält, wie es sich verhält —
für alle, die ein Netz planen (Familie, Nachbarschaft, Organisation), nicht
nur ein einzelnes Gerät bedienen wollen. Für die reine Bedienung siehe die
[Bedienungsanleitung](ponsal-bedienungsanleitung.html).*

---

## Begriffe auf einen Blick

| Begriff | Bedeutung |
|---|---|
| **LoRa** | *Long Range* — Funktechnik für große Reichweite bei wenig Daten |
| **PSK** | *Pre-Shared Key* — gemeinsames Geheimnis, das ein Gerät zu einem Kanal gehören lässt |
| **AES-256** | Verschlüsselungsverfahren, mit dem jede Nachricht automatisch gesichert wird |
| **ECDH** | Verfahren, mit dem zwei Geräte beim Pairing einen Schlüssel austauschen, ohne ihn selbst unverschlüsselt zu senden |
| **SF (Spreading Factor)** | Wie "gedehnt" ein Funksignal gesendet wird — höherer Wert = mehr Reichweite, aber langsamer |
| **dBm** | Maßeinheit für Signalstärke (je näher an 0, desto stärker) |
| **Airtime** | Wie lange eine einzelne Nachricht tatsächlich in der Luft ist |
| **Duty Cycle** | Pflicht-Sendeanteil — wie viel Prozent der Zeit ein Gerät maximal senden darf |
| **Mesh** | Maschennetz — Geräte leiten Nachrichten füreinander weiter, es gibt keinen zentralen Server |
| **Flooding** | Die Weiterleitungsmethode von ProjektX: eine Nachricht geht an alle in Reichweite, nicht auf einem festgelegten Weg |

Jeder Begriff wird unten an der Stelle, an der er inhaltlich wichtig wird,
nochmal im Zusammenhang erklärt — die Tabelle ist zum Nachschlagen gedacht,
nicht als Voraussetzung zum Weiterlesen.

---

## Was ist LoRa

LoRa steht für *Long Range* — eine Funktechnik, die entwickelt wurde, um
mit wenig Sendeleistung große Entfernungen zu überbrücken. Das
funktioniert ohne Mobilfunkmast und ohne Internetanbindung, weil zwei
Geräte direkt miteinander funken, so wie ein Walkie-Talkie — nur mit sehr
viel größerer Reichweite (mehrere Kilometer, je nach Gelände) und sehr viel
weniger Daten pro Nachricht.

Der Grund dafür liegt in der Modulation selbst: LoRa "dehnt" jedes Bit
einer Nachricht über eine längere Zeit, wodurch der Empfänger es auch bei
sehr schwachem Signal noch erkennen kann. Wie stark gedehnt wird, steuert
der **Spreading Factor (SF)** — ein Wert zwischen 7 und 12:

- **Niedriger SF (z. B. 7):** kurze Sendezeit pro Nachricht, aber geringere
  Reichweite
- **Hoher SF (z. B. 12):** deutlich größere Reichweite, aber jede Nachricht
  braucht viel länger in der Luft

Das ist kein Nachteil, den ProjektX hat — das ist die physikalische
Eigenschaft der Funktechnik selbst, und der Grund, warum es bei ProjektX
mehrere Funk-Profile mit unterschiedlichem SF gibt (siehe unten).

---

## Wie eine Nachricht durchs Netz kommt (Mesh, Flooding)

ProjektX braucht keinen Server, keinen Router, keinen zentralen Knoten.
Stattdessen bilden alle Geräte im selben Kanal ein **Mesh** (Maschennetz):
Jedes Gerät, das eine Nachricht empfängt, sendet sie automatisch noch
einmal unverändert weiter — mit einer kleinen zufälligen Verzögerung
(0–1000 ms), damit nicht alle Geräte gleichzeitig senden und sich
gegenseitig stören. Diese Methode heißt **Flooding**: die Nachricht "flutet"
das Netz, statt einen einzelnen, festgelegten Weg zu nehmen.

Damit das nicht zu einer Endlosschleife wird, gibt es zwei eingebaute
Sicherungen:

- **Duplikatfilter (60 Minuten):** Jede Nachricht hat eine eindeutige
  Kennung. Ein Gerät, das dieselbe Nachricht ein zweites Mal empfängt
  (weil mehrere Nachbargeräte sie weitergeleitet haben), verwirft sie
  einfach, statt sie erneut zu senden.
- **Echo-Vermeidung:** Ein Gerät erkennt seine eigene, ursprünglich
  gesendete Nachricht (über die sogenannte NodeID, abgeleitet aus seiner
  Hardware-Adresse) und leitet sie nicht an sich selbst zurück.

**Was das bewusst nicht kann:** Es gibt kein gezieltes Routing — eine
Nachricht nimmt nicht "den kürzesten Weg" zu einem bestimmten Gerät,
sondern verteilt sich einfach im ganzen erreichbaren Netz. Es gibt auch
keine Zustellbestätigung — ob eine Nachricht tatsächlich bei allen
angekommen ist, lässt sich nicht sicher feststellen. Für kleine Netze
(siehe unten) ist das ein akzeptabler Kompromiss zugunsten von Einfachheit
und Zuverlässigkeit ohne Infrastruktur.

---

## Warum es Sendepausen gibt (Duty Cycle)

Der **Duty Cycle** ist keine Ponsal-eigene Erfindung, sondern eine
gesetzliche Vorgabe für das in Europa genutzte 868-MHz-Funkband: Ein Gerät
darf dort nur einen bestimmten Prozentsatz der Zeit tatsächlich senden,
der Rest muss Pause sein — damit sich viele verschiedene Geräte
(nicht nur ProjektX) das Band teilen können, ohne sich gegenseitig zu
stören.

Ein Rechenbeispiel macht das greifbar: Bei SF12 (Profil "Reichweite")
dauert eine einzelne, längere Nachricht (255 Byte) rund 5 Sekunden
**Airtime** — die Zeit, die sie tatsächlich als Funksignal unterwegs ist.
Bei einem erlaubten Duty Cycle von 1 % bedeutet das: Nach dieser einen
Nachricht muss das Gerät im Schnitt etwa das 99-fache dieser Zeit warten,
bevor es wieder senden darf — in der Praxis rund 4 Minuten Pause. Bei den
Profilen mit niedrigerem SF ist die Airtime pro Nachricht viel kürzer, die
Pause fällt entsprechend kaum auf.

Das erklärt auch, warum das Mesh-Weiterleiten (siehe oben) Sendezeit
"kostet": Jede Weiterleitung ist technisch ein ganz normaler Sendevorgang
und zählt genauso zum eigenen Duty-Cycle-Budget wie eine selbst geschriebene
Nachricht.

---

## Die vier Funk-Profile — und wann welches

| Profil | SF | Subband | Duty Cycle | Wann |
|---|---|---|---|---|
| **Standard** | 9 | M | 1 % | Für den Alltag — in fast allen Fällen die richtige Wahl |
| **Reichweite** | 12 | M | 1 % | Nur zum gezielten Testen der maximalen Distanz — wegen der ~4-Minuten-Pause pro Nachricht nicht für Dauerbetrieb gedacht |
| **Organisation** | 9 | P | 10 % | Eigenes, unabhängiges Netz — siehe Erklärung unten |
| **Schnelle Nachrichten** | 7 | M | 1 % | Wenn viele kurze Nachrichten wichtiger sind als maximale Reichweite |

**Zur Klarstellung bei "Organisation":** Der Name beschreibt einen
typischen Anwendungsfall (Feuerwehr, THW, Gemeinde), ist aber **keine
Zugriffsbeschränkung** — jede Privatperson kann dieses Profil genauso wählen
wie jedes andere. Der eigentliche technische Unterschied ist das
**Subband**: "Organisation" nutzt Subband P statt M, ein eigener,
unabhängiger Frequenzbereich innerhalb des 868-MHz-Bands. Dadurch landet
ein Gerät auf diesem Profil automatisch in einem eigenen Funkraum, getrennt
vom Standard-Privatnetz der Umgebung — praktisch, wenn zwei unabhängige
Netze (z. B. eine Feuerwehr und die umliegenden Privathaushalte) sich
nicht gegenseitig mit Sendezeit belasten sollen, auch wenn ohnehin
unterschiedliche PSKs verhindern würden, dass sie Nachrichten lesen
könnten. Der höhere erlaubte Duty Cycle (10 % statt 1 %) auf diesem
Subband ist ein zusätzlicher praktischer Vorteil für Organisationen mit
höherem Nachrichtenaufkommen — kein Grund, der die Wahl auf sie
beschränkt.

Wichtig, unabhängig vom gewählten Profil: **Geräte mit unterschiedlichem
Funk-Profil können nicht miteinander kommunizieren** — SF und Subband
müssen exakt übereinstimmen.

---

## Kanäle und Netzwerkschlüssel

Ein **Kanal** bei ProjektX ist im Kern nichts anderes als ein **PSK**
(*Pre-Shared Key* — ein gemeinsames, geheimes Schlüsselstück). Wer denselben
PSK hat, kann Nachrichten in diesem Kanal lesen und schreiben; wer ihn
nicht hat, sieht nichts, selbst wenn das Gerät in Funkreichweite ist und
auf demselben Funk-Profil sendet.

Ein Gerät kann mehrere Kanäle gleichzeitig kennen (bis zu 8) — z. B. einen
privaten Familienkanal *und* einen gemeinsamen Dorf-Infokanal. Das
funktioniert nur, solange alle diese Kanäle **dasselbe Funk-Profil**
nutzen (siehe oben) — ein Gerät kann nicht gleichzeitig auf zwei
verschiedenen SF/Subband-Kombinationen lauschen.

---

## Wann welche Kanäle sinnvoll sind

Drei typische Szenarien, an denen sich die Entscheidung orientieren lässt:

- **Familie (3–5 Geräte):** Ein einzelner Kanal reicht. Kein Grund für
  mehr Komplexität.
- **Nachbarschaft/Dorf:** Privater Familienkanal für den eigenen Haushalt
  *plus* ein gemeinsamer Infokanal für alle — beide auf dem
  Standard-Profil, damit jedes Gerät beide gleichzeitig empfangen kann.
- **Organisation (Feuerwehr, THW, Gemeindeverwaltung):** Eigener Kanal auf
  dem "Organisation"-Profil, bewusst getrennt vom privaten Standard-Netz
  der Umgebung — aus den oben genannten Gründen (eigener Funkraum, höherer
  Duty Cycle).

---

## Warum das Netz klein bleiben soll

ProjektX ist bewusst für kleine Netze gedacht — bis zu etwa **20 Geräte**
gelten als guter Richtwert. Der Grund liegt direkt im Flooding-Prinzip von
oben: Jedes zusätzliche Gerät im selben Kanal sendet nicht nur für sich
selbst, sondern leitet auch die Nachrichten aller anderen weiter. Mehr
Geräte bedeuten also nicht nur mehr eigene Nachrichten, sondern
überproportional mehr Weiterleitungs-Verkehr — und damit mehr
Duty-Cycle-Verbrauch für alle, ohne dass eine einzelne Nachricht dadurch
schneller ankommt. ProjektX ist kein Ersatz für große Infrastruktur,
sondern für den überschaubaren Kreis gedacht, für den es entwickelt wurde.

---

## Sicherheit in einem Absatz

Jede Nachricht wird automatisch mit **AES-256** verschlüsselt — dem
Verfahren, das den eigentlichen Inhalt unlesbar macht für jeden ohne den
passenden PSK. Beim Pairing (siehe Bedienungsanleitung) wird dieser PSK
nie unverschlüsselt über Funk übertragen: Zwei Geräte handeln ihn per
**ECDH** aus, einem Verfahren, bei dem beide Seiten unabhängig voneinander
denselben gemeinsamen Schlüssel berechnen können, ohne ihn jemals
tatsächlich zu senden. Ein mithörendes drittes Gerät bekommt dadurch selbst
bei aktivem Pairing keinen Zugriff auf den PSK.

---

*Für die praktische Bedienung — Kanal einrichten, Geräte pairen, Werksreset
— siehe die [Bedienungsanleitung](ponsal-bedienungsanleitung.html).*
