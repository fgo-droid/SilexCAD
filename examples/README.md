# Modèles d'exemple

Trois conceptions natives Silex, à ouvrir avec **Fichier › Ouvrir** ou en ligne de commande :

```powershell
.\build\bin\Release\silex.exe examples\plaque_percee.silex
```

| Fichier | Contenu | Opérations |
| --- | --- | --- |
| `plaque_percee.silex` | Plaque 100 × 60 × 8 mm, 4 perçages Ø8 et un alésage central Ø20 | esquisse, extrusion, congés R8 sur les 4 arêtes verticales, esquisse sur la face supérieure, enlèvement traversant |
| `boitier.silex` | Boîtier ouvert 80 × 50 × 30 mm, parois et fond de 3 mm | esquisse, extrusion, congés R4, poche esquissée sur le dessus, enlèvement de 27 mm |
| `bague_epaulee.silex` | Bague à épaulement Ø60 / Ø28, alésage Ø10, hauteur 40 mm | demi-profil sur le plan XZ, révolution 360° autour d'une ligne de construction, congé R2 à l'épaulement, chanfrein 1 mm |

Les esquisses sont entièrement contraintes et cotées. Seule la ligne d'axe de la bague garde deux degrés
de liberté le long de l'axe, ce qui est voulu. Les congés, les chanfreins et les esquisses posées sur une
face s'appuient sur des **références topologiques persistantes**.

Pour vérifier le nommage topologique, change une cote et regarde le reste suivre. Par exemple, dans
`plaque_percee.silex` :

1. Double-clique sur « Contour plaque » dans la timeline.
2. Double-clique sur la cote 100 et tape `140`.
3. Termine l'esquisse.

Les congés restent sur les bons angles. Les perçages restent sur la face supérieure, à leurs positions
cotées depuis l'origine.

Le test `Examples.*` de la suite `silex_tests` recalcule ces trois fichiers et vérifie leurs volumes.

## Cas d'école annotés (mode guidé)

Chaque fichier est la solution de référence d'un parcours du panneau **Learn** (`F1` > *Ouvrir l'exemple annoté*)
et porte une annotation FR / EN par étape pour la **visite guidée**. Ils sont régénérés par l'API MCP/IPC avec
`python tools/examples/build_examples.py` ; le test `Finitions.EachAnnotatedExampleCompletesItsLesson` rejoue chaque
timeline étape par étape et vérifie que toutes les étapes du parcours se valident.

| Fichier | Parcours | Fonctions montrées |
| --- | --- | --- |
| `equerre_annotee.silex` | 1 — Équerre (débutant) | esquisse contrainte, extrusions, perçage, congés |
| `de_parametrique.silex` | 4 — Dé à jouer (débutant) | rectangle par le centre, paramètre `cote`, esquisses sur 6 faces, répétition 2D, congés R2 sur 12 arêtes |
| `pince_serrage.silex` | 5 — Pince de serrage (intermédiaire) | profil en C + congés 2D, 2 nervures, bossage Ø18, taraudage M8 réel (jeu 0,15) |
| `poulie_gorge.silex` | 6 — Poulie (intermédiaire) | demi-profil + révolution autour de X, alésage Ø12, rainure de clavette (oblong), 6 trous d'allègement en répétition circulaire |
| `palier_semelle.silex` | 7 — Support de roulement (intermédiaire) | demi-pièce + symétrie YZ, oblong de réglage, logement Ø26 concentrique, trou de graissage |
| `raccord_carre_rond.silex` | 8 — Raccord carré-rond (avancé) | plan décalé +60, lissage 60 × 60 → Ø40, coque 2 mm ouverte aux deux bouts |
| `coude_brides.silex` | 9 — Coude à brides (avancé) | balayage le long d'un arc R45, section pilotée par `diametre_tube` / `epaisseur_paroi`, 2 brides, 2 × 4 trous en répétition circulaire |

## Boîtier Raspberry Pi 4

`boitier_raspberry_pi4.silex` : boîtier 94 × 64 × 30 mm à couvercle emboîtable, créé par l'assistant boîtier.
Il contient la carte de la bibliothèque (verrouillée) posée sur 4 entretoises, les ouvertures de tous les
connecteurs (Ethernet, 2 × USB, USB-C, 2 × micro HDMI, audio, micro SD), des fentes d'aération et le logo
gravé sur le couvercle. Les dimensions intérieures sont des paramètres (`Ctrl+P`). Régénérable par
`python tools/examples/build_rpi_case.py`.
