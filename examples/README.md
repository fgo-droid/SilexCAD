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
