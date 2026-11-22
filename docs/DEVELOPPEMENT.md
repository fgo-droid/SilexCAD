# Silex — guide technique (architecture, compilation, MCP)

> Pour la présentation générale, voir le [README](../README.md). Ce document s'adresse aux développeurs et aux curieux.

Silex associe la puissance d'un noyau B-Rep ouvert (**OpenCascade**, solveur **PlaneGCS**) à un flux de travail
contextuel unifié inspiré de Fusion : pas d'ateliers, une timeline horizontale, des aperçus transparents en
temps réel et un nommage topologique persistant dès le premier jour.

Ce dépôt implémente les **phases 1 à 3 de la feuille de route, ainsi que le TNP** décrits dans
`guidelines_d_architecture_et_de_developpement_cao.md`.

## Architecture

```
src/
  model/    silex_model    Données pures (ni OCCT ni Qt) : arbre paramétrique, DocState immuable,
                           commandes (Command pattern / MVI), undo/redo, DAG, JSON, maillages de rendu
  sketch/   silex_sketch   Solveur PlaneGCS (third_party/planegcs, FreeCAD 1.0) + auto-contraintes
  kernel/   silex_kernel   OCCT : ParametricFeature (plugins), recalcul incrémental annulable dans un
                           thread dédié, nommage topologique persistant, maillage, STEP
            KernelEngine.h seule API publique du noyau — sans aucun type OCCT
  app/      silex          Qt 6 Widgets + viewport QRhi (D3D11/12, Vulkan, Metal)
tests/                     GoogleTest : validité BRepCheck, volumes à 1e-6, survie TNP
```

Règles d'architecture appliquées mécaniquement :

* **Aucun OCCT dans l'UI.** Les répertoires d'en-têtes OCCT sont `PRIVATE` à `silex_kernel`, donc
  `src/app` ne peut physiquement pas inclure `TopoDS_Shape`. L'UI ne manipule que des UUID, des noms
  persistants et des maillages (`model/RenderModel.h`).
* **Exécution non bloquante.** `KernelEngine` recalcule dans un worker. Chaque nouvelle requête annule la
  précédente : `std::stop_token` est relié à l'indicateur de progression OCCT (*user break*). Le maillage
  est fait dans le worker.
* **Erreurs non fatales.** Une fonction qui échoue est marquée en rouge dans la timeline, avec son
  message, et le dernier état géométrique valide est conservé.

### Nommage topologique persistant (TNP)

Chaque face ou arête reçoit un nom sémantique dérivé de l'historique OCCT (`Modified` / `Generated` /
`IsDeleted`), jamais d'un index :

| Entité | Nom |
| --- | --- |
| courbe d'esquisse | `<sketchId>/<geoId>` |
| face latérale d'extrusion | `<featId>:side(<sketchId>/<geoId>)` |
| faces de fermeture | `<featId>:start`, `<featId>:end` |
| face de congé / chanfrein | `<featId>:fillet(<nom d'arête>)` |
| arête | `E[<face A>\|<face B>]` |

Pour résoudre une référence, Silex cherche d'abord le nom exact. Si plusieurs entités le portent, il les
départage par une signature géométrique (centroïde, normale ou tangente, aire ou longueur). Si le nom a
disparu, il se rabat sur la géométrie la plus proche et lève un avertissement. Les tests `TNP.*`
modifient une cote d'esquisse en amont et vérifient que le congé, le chanfrein et l'esquisse posée sur une
face survivent.

### Viewport

Chaque image est rendue en trois passes sur QRhi :

1. une passe opaque (grille, corps, arêtes) vers une texture de couleur et une texture de profondeur ;
2. une passe de **transparence indépendante de l'ordre** (*weighted blended OIT*) pour les profils, les
   aperçus d'opération et les plans d'origine ;
3. une composition finale (dégradé, FXAA, résolution de l'OIT).

La sélection se fait par lancer de rayon sur un BVH pour les faces et les profils, et par distance en
espace écran pour les arêtes.

## Fonctionnalités

* Esquisse sur les plans d'origine ou sur une face plane, avec une référence persistante qui suit la face.
* Outils d'esquisse : ligne (polyligne), rectangle, cercle, arc, cotes éditables sur place, contraintes
  H, V, coïncidence, parallèle, perpendiculaire, tangente, égalité, blocage, géométrie de construction.
* Contraintes automatiques pendant le tracé : accrochage aux points et aux courbes, horizontale ou
  verticale, tangente en sortie d'arc.
* Code couleur des degrés de liberté : **bleu** (sous-contraint), **blanc** (totalement contraint),
  **rouge** (sur-contraint). Les contraintes qui sur-contraindraient l'esquisse sont refusées.
* Manipulation directe : on fait glisser points et courbes, et PlaneGCS résout en direct.
* Opérations : extrusion (nouveau corps, union, enlèvement, intersection ; un côté, inversée ou
  symétrique), révolution (axe X/Y de l'esquisse ou ligne de l'esquisse), congé, chanfrein, import STEP.
* **v0.2** :
  * **Shell** (`H`) : coque avec faces ouvertes, épaisseur, direction Inside / Outside.
  * **Mirror** (`M`) : symétrie de corps ou de fonctions par rapport à un plan d'origine ou à une face plane,
    avec fusion automatique si la copie touche l'original.
  * **Linear Pattern** (`Maj+P`, une ou deux directions) et **Circular Pattern** (`Maj+C`) : répétitions de
    corps ou de fonctions (perçages, bossages). Chaque occurrence est indexée dans les noms TNP, si bien que
    les congés et chanfreins posés sur les occurrences existantes survivent à un changement du nombre
    d'occurrences.
  * Fichiers au format `schema_version: "0.2"`, avec rétrocompatibilité totale à l'ouverture des fichiers 0.1.
* **v0.3** (feuille de route en 4 lots) :
  * **Lot 1 — analyse mécanique & paramétrage**
    * **Paramètres globaux** (`Ctrl+P`) : variables nommées (`epaisseur_paroi = 2.0`, `jeu_montage = 0.25`,
      `entraxe_x = largeur - 2 * marge`), évaluateur d'expressions (`+ - * / ^ ( )`, `sin cos tan` en degrés,
      `sqrt abs min max round`, unités `mm cm m in deg`, références croisées, détection des cycles). Tout champ
      numérique accepte une expression : cotes d'esquisse, extrusion, congé, coque, répétitions. Modifier un
      paramètre réécrit les champs liés, re-résout les esquisses et recalcule la timeline en cascade.
    * **Mesure** (`I`) : distance minimale (`BRepExtrema_DistShapeShape`) et ΔX ΔY ΔZ, angle entre faces planes
      ou arêtes droites, Ø / R / centre des arêtes circulaires et cylindres, cote temporaire dans la vue.
      **Propriétés physiques** permanentes dans l'inspecteur (volume, aire, boîte englobante, masse estimée).
    * **Interférences** (`Maj+I`) : volumes de chevauchement (`BRepAlgoAPI_Common`, > 1e-4 mm³) en rouge vif
      dans une vue rayons X, jeu fonctionnel FDM : rouge (collision), orange (< 0,15 mm), vert (≥ 0,15 mm).
    * **Vue en coupe** (`Alt+S`) : plan XY / XZ / YZ ou face plane, curseur de décalage, `Flip`. Coupe faite dans
      les shaders (temps réel), matière coupée peinte en plein ; ni le B-Rep ni la timeline ne sont modifiés.
  * **Lot 2 — Sweep (`Maj+W`) & Loft (`Maj+L`)** : `BRepOffsetAPI_MakePipeShell` (profil + trajectoire esquisse ou
    chaîne d'arêtes, modes `CorrectedFrenet` / `Frenet` / `Fixed`) et `BRepOffsetAPI_ThruSections` (sections
    ordonnées, `Solid`, `Ruled`), opérations booléennes, TNP (`side(...)`, `start`, `end`). Les esquisses ont un
    **décalage de plan** (`offset`) pour empiler les sections.
  * **Lot 3 — mise en plan 2D** (`Ctrl+D`) : projection HLR (`HLRBRep_Algo` + `HLRBRep_HLRToShape`) en 3 vues,
    1er dièdre (face XZ, dessus XY dessous, droite YZ à gauche), arêtes cachées en tirets, axes en trait mixte,
    feuille A4 / A3 avec cadre et cartouche éditable, cotes linéaires / Ø / R accrochées aux sommets et arcs et
    recalées quand le modèle change, export PDF (`QPdfWriter`) et SVG.
  * **Lot 4 — matériaux & 3MF** : matériau par corps (PLA, PETG, ABS, résine translucide, PCB, aluminium, acier
    ou couleur RGBA / densité personnalisées), corps translucides rendus en OIT ; export **3MF** (`Ctrl+3`) :
    un objet par corps avec nom et couleur, unité millimètre, maillages étanches, métadonnées.
  * Fichiers au format `schema_version: "0.3"` (`parameters`, `bodies[].material`, `drawing`) ; les fichiers
    0.1 et 0.2 s'ouvrent toujours.
* **Esquisse Pro** (mise à niveau du sketcher, module pur 2D `src/sketch/SketchTools`) :
  * Contraintes **Milieu** (`M`, point + ligne ou arc), **Symétrie** (`S`, 2 points / lignes / cercles / arcs + axe),
    **Concentrique** (`O`) et **Colinéaire**. Les tangences en bout de courbe passent par `AngleViaPoint` (comme
    FreeCAD), ce qui évite les fausses redondances tangence + coïncidence.
  * Formes pré-contraintes : **rectangle par centre** (`Maj+R`, diagonales de construction + Milieu : 2 cotes),
    **oblong** (`U`, centre à centre ou par centre global, côtés tangents, demi-cercles égaux, axe de construction),
    **polygone régulier** (3 à 12 côtés, inscrit ou circonscrit).
  * Édition 2D : **Ajuster** (`T`, portion supprimée en rouge pointillé au survol, contraintes valides conservées),
    **Prolonger**, **congé d'esquisse** (`Maj+F`, cotes de longueur converties au coin virtuel), **Décaler**
    (`Maj+O`, une seule cote pilote toute la copie), **Miroir 2D** (contraintes Sym) et **Répétition 2D**
    linéaire / circulaire (pas porté par des lignes de construction cotées). La tangente passe sur `Maj+T`.
  * Cotes à la volée (champs flottants, `Tab` / `Entrée`, expressions de paramètres acceptées), badges de
    contraintes (survol = éléments liés, clic + `Suppr`), grille magnétique **Snap** 0,5 / 1 / 5 mm dans la barre
    d'état (`Alt` la désactive), bouton **Show under-constrained**.
  * MCP : `sketch_add_geometry` accepte `center_rectangle`, `slot`, `polygon`, `sketch_fillet`, `offset`, `trim`,
    `extend`, `mirror`, `linear_pattern`, `circular_pattern` (option `"dimension": true`) ;
    `sketch_add_constraints` accepte `midpoint`, `symmetry` (`"axis"`), `concentric`, `collinear`.
* **Ergonomie & navigation** :
  * **ViewCube** (`src/app/ViewCube`) en haut à droite de la vue : 6 faces, 12 arêtes et 8 coins cliquables avec
    surbrillance, bouton Maison (iso + cadrage), flèches de roulis à 90° en vue alignée, animation SLERP de ~200 ms,
    libellés selon la langue du système.
  * **Press/Pull** (`Q`) : sur un profil fermé = extrusion ; sur une face plane = fonction `OffsetFace`
    (`BRepPrimAPI_MakePrism` le long de la normale, fusion si > 0, enlèvement si < 0), flèche orange, aperçu bleu / rouge,
    étape éditable de la timeline.
  * **Primitives** `Box` (`Maj+B`) et `Cylinder` (`Maj+Y`) sans esquisse : plan XY / XZ / YZ ou face plane (repère au
    centre de la face), centre, dimensions, hauteur, opération New body / Join / Cut.
  * **Barre d'état** : voyant MCP (gris prêt / vert agent connecté / orange IPC désactivé), nombre de triangles, temps
    de recalcul, bouton d'accrochage grille (Off, 0,5, 1, 2, 5 mm).
  * MCP : `create_box`, `create_cylinder`, `press_pull`.
* **v0.4** (feuille de route en 4 lots, `schema_version: "0.4"`, fichiers 0.1 à 0.3 toujours lisibles) :
  * **Lot 0 — corps & faces** : `Suppr` sur un corps = étape `RemoveBody` (historique intact, annulable) ; `Suppr`
    sur des faces = `DefeatureFace` (`BRepAlgoAPI_Defeaturing`, cicatrisation, STEP sans historique compris) ;
    `V` masquer / montrer, œil dans l'Inspecteur, **Isolate / Unisolate** ; **Split Body** (`BOPAlgo_Splitter`,
    `<Nom>_1` / `<Nom>_2`, TNP par moitié) ; **Move / Copy** (`TransformBody`, gizmo 3 flèches + 3 arcs, ΔX…Rz,
    *Create copy*). MCP : `remove_body`, `defeature_faces`, `split_body`, `transform_body`.
  * **Lot 1 — mécanique** : **plans de construction** décalé / incliné / milieu (dossier *Construction*, utilisables
    pour esquisses, primitives et Split) ; **nervure** depuis une ligne ouverte d'esquisse (remplit l'espace jusqu'aux
    parois) ; **Align** face à face avec jeu ou coaxial (étape `TransformBody` recalculée). MCP :
    `create_construction_plane`, `create_rib`, `align_bodies`.
  * **Lot 2 — impression 3D** : **Printability** (porte-à-faux 45-60° orange, > 60° rouge, parois < 0,8 mm en magenta,
    direction d'impression au choix) ; **filetages ISO réels** M3-M12 (pas normal / fin) avec **jeu FDM** : vis et
    écrou coaxiaux s'engrènent toujours (phase calée sur le repère monde), mode cosmétique. MCP : `check_printability`,
    `create_thread`.
  * **Lot 3 — pédagogie** : panneau **Apprendre / Mode guidé** (`F1`) avec 3 parcours JSON auto-validés
    (`resources/tutorials`), bouton **Hint** (l'outil attendu clignote) ; **Expliquer** dans l'esquisse (SketchCoach :
    sondage du solveur, phrase claire par groupe d'éléments) ; explication des étapes en erreur ; **visite guidée** des
    fichiers annotés ; MCP `get_pedagogical_context` (mode tuteur).
  * **Finitions** : barre *Modèle* regroupée (`Create ▾`, `Modify ▾`, `Pattern ▾`, `Construct ▾`, `Inspect ▾`) qui
    tient en 1366 px ; sélection d'esquisse cyan 3 px, survol jaune (2D et 3D) ; **sélection par cadre** (→ fenêtre,
    ← capture) en esquisse et en 3D ; `N` vue normale au plan d'esquisse, `Ctrl+A`, clic droit = contraintes
    compatibles avec la sélection ; infobulles bilingues des contraintes.
  * **Bibliothèque pédagogique** : **9 parcours** bilingues FR / EN classés par niveau (dé paramétrique, pince de
    serrage, poulie à gorge, support de roulement, raccord carré-rond, coude à brides en plus des 3 premiers), chacun
    avec son **exemple annoté** dans `examples/` (visite guidée FR / EN), régénérable par
    `python tools/examples/build_examples.py`. Le test `Finitions.*` valide chaque JSON et rejoue chaque exemple
    étape par étape jusqu'à la fin de son parcours.
* **v0.5 — Makers & Pédagogie** (5 lots) :
  * **Visserie & composants** : outil **Hole / Insert** (`Maj+H`, étape `FastenerHole`) — passage serré / normal,
    lamage DIN 912, fraisage 90° DIN 7991, logement d'insert laiton (M3 → Ø4,0 × 5,7, chanfrein 0,4), écrou
    prisonnier ISO 4032 avec jeu FDM, M2 à M6, plusieurs trous par étape, accroche aux axes de trous. **Bibliothèque
    de 9 composants** (`resources/components/*.json` + `.step` simplifiés aux cotes officielles, régénérables par
    `python tools/components/build_components.py`) : insérés comme **corps de référence verrouillés** (matériau,
    jamais modifiés, imprimés ni exportés ; un Join sans cible vise la pièce, pas la carte).
  * **Impression 3D** : plateau **Bed** (MK4 / Ender 3, Bambu, Prusa Mini, personnalisé) avec alerte de dépassement ;
    **Lay Flat on Bed** (étape recalculée) ; **Split Body + pions d'alignement** (logements avec jeu radial 0,20) ;
    **assistant boîtier** qui écrit une timeline commentée et paramétrée (lèvre, 4 colonnettes M3 + inserts, couvercle
    coulissant), en un seul Annuler.
  * **Défis de dépannage** : 5 pièces cassées dans Learn, validées par critère + **test de robustesse** (Silex change une
    cote sur une copie et recalcule) ; régénérables par `python tools/examples/build_repairs.py`.
    **Fantôme avant / après** filaire pendant l'édition d'une cote, d'une étape ou d'un paramètre.
  * **Interface Essentiel / Complet** (7 commandes pour les débutants) et **cartes d'intention** illustrées
    avant → après (survol 1,2 s) sur les barres Modèle et Esquisse.
  * **Formateurs** : **enregistreur de leçons** (timeline → leçon JSON avec critères automatiques, consignes FR / EN,
    pack `.json + .silex`, « Mes leçons ») ; **tuteur IA socratique** (`--tutor-mode` ou bouton Learn) : le serveur
    IPC / MCP n'accepte plus que les commandes d'observation.
* Combinaison de corps existants : union, soustraction et intersection, en conservant ou non les outils.
  * Pour sélectionner une pièce entière, double-clique dessus (Ctrl+double-clic pour en ajouter
    d'autres), ou coche-la dans l'inspecteur.
  * Avec 2 pièces ou plus sélectionnées, le menu radial et l'inspecteur proposent « Fusionner »,
    « Soustraire » et « Intersecter » en un clic. La première pièce sélectionnée, en orange, sert de cible.
  * La touche `B` ouvre l'éditeur Combiner complet.
* Export STEP et STL depuis l'interface (déflexion du maillage réglable pour le STL).
* Aperçu transparent en direct, avec une flèche d'extrusion déplaçable dans la vue (Ctrl pour arrondir
  au millimètre).
* Timeline horizontale : marqueur de retour arrière déplaçable, réordonnancement par glisser (refusé s'il
  casse une dépendance), suppression, désactivation, renommage, édition par double-clic.
* Menu radial : on maintient le clic droit et on lance la souris dans une direction. Raccourcis
  contextuels.
* Fichiers `.silex` en JSON lisible et versionnable ; le B-Rep importé y est embarqué. Export STEP AP214.

### Raccourcis

| Contexte | Touches |
| --- | --- |
| Corps & faces (v0.4) | `Suppr` supprimer un corps / des faces (étape annulable) · `V` masquer / montrer · clic droit : Isolate, Move / Copy, Align, Split body |
| Apprendre (v0.4) | `F1` mode guidé · Expliquer (esquisse) · Hint · Guided tour |
| Makers (v0.5) | `Maj+H` perçage / insert · Create ▾ : Insert Component, Enclosure Wizard · Modify ▾ : Lay Flat on Bed · Bed (barre d'état) · profil Essentiel / Complet (en haut à droite) |
| Sélection | glisser dans le vide : → fenêtre · ← capture · `Ctrl+A` (esquisse) · `N` vue normale (esquisse) · clic droit : contraintes compatibles |
| Modélisation directe | `Q` pousser / tirer · `Maj+B` boîte · `Maj+Y` cylindre · ViewCube (faces, arêtes, coins, Maison) |
| Modèle | `S` esquisse · `E` extrusion · `R` révolution · `F` congé · `C` chanfrein · `H` coque · `M` symétrie · `Maj+P` / `Maj+C` répétitions · `Maj+W` balayage · `Maj+L` lissage · `W`/`Home` cadrer · `0 1 3 7` vues |
| Analyse (v0.3) | `I` mesure · `Maj+I` interférences · `Alt+S` coupe · `Ctrl+P` paramètres · `Ctrl+D` mise en plan · `Ctrl+3` export 3MF |
| Esquisse | `L` ligne · `R` rectangle · `Maj+R` rectangle par centre · `C` cercle · `A` arc · `U` oblong · `D` cote · `X` construction · `Suppr` · `Entrée` terminer |
| Édition 2D | `T` ajuster · `Maj+F` congé · `Maj+O` décaler · valeur + `Tab` / `Entrée` pendant le tracé · `Alt` ignore la grille |
| Contraintes | `H` `V` `I` `P` `K` `Q` `F` · `Maj+T` tangente · `M` milieu · `S` symétrie · `O` concentrique |
| Navigation | molette : zoom au curseur · clic milieu : pan · Maj+clic milieu ou Alt+clic gauche : orbite |

## Compilation (Windows)

Prérequis : Visual Studio 2022 avec la charge **Développement Desktop en C++**, et
[vcpkg](https://github.com/microsoft/vcpkg) (`VCPKG_ROOT` doit pointer dessus).

```powershell
$env:VCPKG_ROOT = "C:\Projects\vcpkg"
cmake --preset windows                 # compile les dépendances via vcpkg.json (long la 1re fois)
cmake --build --preset release
ctest --preset release
.\build\bin\Release\silex.exe --demo
```

Options de `silex.exe` : `--demo` (pièce de démonstration), `--screenshot out.png` (capture puis
quitte), `--edit <fonction>` et `--tool measure|interference|section|drawing|parameters|properties`
(ouvrent un éditeur ou un outil, pour les captures), `--headless` (pilotage MCP seul), `fichier.silex`.
v0.5 : `--tutor-mode` (tuteur IA socratique), `--tool essential|bed|hole|components|enclosure|recorder|ghost|card:<outil>`
(captures de documentation, plusieurs séparés par des virgules).

### Outils MCP ajoutés en v0.5

`create_fastener_hole`, `list_components`, `insert_component`, `lay_flat_on_bed`, `check_build_volume`,
`create_enclosure`, `split_body(..., pegs=True)`, `record_lesson` (IPC). Le pont `mcp_server.py --tutor-mode`
(ou `SILEX_TUTOR=1`) n'expose que les outils de lecture.

### Outils MCP ajoutés en v0.3

`set_parameter`, `get_parameters`, `delete_parameter`, `update_feature_parameter(..., expression=...)`,
`measure`, `check_interference`, `set_body_material`, `sweep`, `loft`, `export_3mf`, `export_drawing`,
`create_sketch(..., offset_mm=...)`.

Le preset `unix` compile sous Linux et macOS avec la même chaîne vcpkg.

## Sauvegarde (NAS) et recompilation rapide

Le dossier de travail pèse environ 23 Go, surtout à cause des fichiers intermédiaires de vcpkg. Pour
recompiler, il suffit d'environ **1,1 Go** :

```powershell
.\tools\backup.ps1 -Destination \\NAS\partage\Silex-dev
```

Le script copie en miroir, et seulement ce qui a changé si tu le relances :

| Copié vers… | Taille | Contenu |
| --- | --- | --- |
| `Silex\` | ~20 Mo | sources, docs, exemples, scripts |
| `Silex\vcpkg_installed\` | ~510 Mo | dépendances **déjà compilées** (OCCT, Qt…), sans les symboles `.pdb` |
| `vcpkg\` | ~36 Mo | l'outil vcpkg (fichier de toolchain et ports figés par `builtin-baseline`) |
| `vcpkg-archives\` | ~585 Mo | cache binaire vcpkg, en plan B (option `-NoArchives` pour ne pas le copier) |

Restauration et recompilation sur n'importe quel PC Windows équipé de Visual Studio 2022 avec la charge C++ :

```powershell
robocopy \\NAS\partage\Silex-dev C:\Projects /E
C:\Projects\Silex\tools\build.ps1 -Offline -Test
```

`-Offline` ne lance jamais vcpkg : seul Silex est compilé. C'est vérifié depuis une copie : compilation
complète et 39 tests réussis en **moins de 2 minutes**. Le script trouve tout seul le dossier `vcpkg\`
placé à côté de `Silex\`.

**Plan B**, si les dépendances doivent être régénérées (autre version de Visual Studio, `vcpkg.json`
modifié…) : indique le cache binaire à vcpkg, puis compile normalement, sans `-Offline`. Les paquets dont la
signature correspond sont restaurés depuis le cache en quelques minutes, au lieu d'être recompilés pendant
environ 1 h 30.

```powershell
$env:VCPKG_DEFAULT_BINARY_CACHE = "C:\Projects\vcpkg-archives"
C:\Projects\Silex\tools\build.ps1 -Test
```

## Pilotage par une IA (MCP)

Silex peut être piloté par un client MCP (Claude Code, Gemini CLI…).

```
client MCP ──stdio──► tools/mcp_bridge/mcp_server.py ──\\.\pipe\cad_mcp──► silex(.exe)
                       (FastMCP, Python)                 (JSON par ligne)     IpcServer + IpcCommandDispatcher
```

* `silex --headless` démarre sans fenêtre. Il sert les requêtes IPC et fait le rendu QRhi hors écran
  (`capture_viewport`). L'application graphique écoute aussi sur ce pipe : on voit alors l'agent travailler
  en direct. `--pipe <nom>` change le nom du point d'accès ; `--no-ipc` désactive le serveur.
* La passerelle démarre `silex --headless` toute seule si aucune instance n'écoute. Le stdout de Silex est
  redirigé, donc les journaux OCCT et Qt ne polluent jamais le canal MCP.
* Outils disponibles :
  * inspection : `get_model_tree`, `inspect_body`, `query_topology` (identifiants TNP persistants),
    `capture_viewport` ;
  * esquisse : `create_sketch`, `sketch_add_geometry`, `sketch_add_constraints` ;
  * opérations : `extrude`, `revolve`, `fillet_or_chamfer`, `shell`, `combine`, `mirror`, `linear_pattern`,
    `circular_pattern` ;
  * historique et paramètres : `update_feature_parameter`, `undo`, `redo` ;
  * échanges : `import_step`, `export_step`, `export_stl`, `save_design` ; mesure : `list_solids`.
* Une modification qui casse la reconstruction est refusée et annulée. L'agent ne laisse donc jamais de
  fonction en erreur derrière lui.
* Cycle documentaire : `new_document`, `open_document(path)`, `get_active_document_info` (chemin, version de
  schéma, statut modifié, nombre d'étapes, de corps, d'esquisses). Les deux premiers refusent d'écraser des
  modifications non enregistrées (`DOCUMENT_DIRTY`) sauf avec `discard_changes=true`.
* `begin_command_group(name)` / `end_command_group()` : les opérations de l'agent deviennent **une seule**
  étape d'annulation pour l'utilisateur (`Ctrl+Z`). Une opération en échec dans le groupe est annulée seule.
* `verify_dimensions(body_id, [x, y, z], tolerance_mm)` : encombrement fini exact (après congés et
  chanfreins) comparé aux cotes nominales, axe par axe.
* Erreurs structurées : chaque erreur IPC a la forme
  `{"code": -32602, "error_code": "UNKNOWN_ID", "message": "...", "hint": "..."}`. Codes : `PARSE_ERROR`,
  `INVALID_REQUEST`, `METHOD_NOT_FOUND`, `INVALID_PARAMS`, `MISSING_PARAMETER`, `UNKNOWN_ID`,
  `REBUILD_FAILED` (avec l'explication pédagogique en `hint`), `DOCUMENT_DIRTY`, `FILE_NOT_FOUND`,
  `INVALID_DOCUMENT`, `GROUP_ALREADY_OPEN`, `NO_GROUP_OPEN`, `IO_ERROR`, `TUTOR_MODE_REFUSED`,
  `OPERATION_FAILED`, `INTERNAL_ERROR`. Le pont MCP les transmet sous la forme `[CODE] message / Hint: …`.

Installation :

```powershell
python -m pip install --user -r tools\mcp_bridge\requirements.txt
```

Ensuite, déclare le serveur dans le client MCP en t'inspirant de `tools/mcp_bridge/mcp_config.example.json`
(remplace `<SILEX_DIR>` par le dossier de `silex.exe`). La compilation copie `mcp_server.py` à côté de
`silex.exe` : le pont y trouve l'exécutable sans aucun chemin absolu. Ordre de recherche : `SILEX_EXE`,
le dossier du script, le `PATH`, puis `build/bin/Release` d'un dépôt source.

Auto-test du pont, sans client MCP :

```powershell
python mcp_server.py --check
```

Il vérifie Python, les modules `mcp` et `pywin32`, le binaire, la connexion au pipe (instance déjà lancée,
ou instance headless privée démarrée puis arrêtée) et la version du protocole IPC (2).
Le test `Ipc.AgentBuildsDrilledFilletedBox` couvre la chaîne de bout en bout : il construit un boîtier
percé avec congés via le pipe, puis vérifie la validité, le volume, la survie TNP, l'annulation, la
capture et l'export STEP/STL.

## v0.7 — Trio Makers : texte, import SVG/DXF, calque image

* **Texte paramétrable** (`Create ▾ > Text (Emboss / Engrave)`, `Maj+T` hors esquisse, clic droit sur une
  face) : relief (profondeur > 0) ou gravure (< 0) sur une face plane, un plan d'origine ou de construction.
  Le texte accepte des `{expressions}` sur les paramètres globaux : `"Boîtier v{version} - {largeur}x{hauteur} mm"`,
  `{expr:2}` force 2 décimales, `{{` et `}}` donnent des accolades. Il est recalculé à chaque changement de
  paramètre. Hauteur (des majuscules), profondeur, rotation et position acceptent aussi des paramètres.
  Police embarquée **Liberation Sans** (portable), polices système ou fichier `.ttf` / `.otf` possibles.
  Avertissement orange si les jambages sont plus fins que la buse de 0,4 mm.
  Les contours des lettres sont lus avec FreeType (l'OpenCascade du projet est compilé sans `Font_BRepFont`),
  courbes de Bézier exactes, trous des lettres (o, e, A…) gérés.
* **Import SVG / DXF** (barre *Esquisse* > *Import SVG/DXF*, `Ctrl+Maj+I`) : SVG `path` (M L H V C S Q T A Z),
  `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`, transformations et unités ; DXF ASCII `LINE`,
  `CIRCLE`, `ARC`, `LWPOLYLINE` / `POLYLINE` (bulges), `SPLINE`, `ELLIPSE`, `$INSUNITS`. Béziers et splines
  approchées à 0,02 mm, extrémités à moins de 0,01 mm fusionnées (coïncidences) : les contours fermés
  s'extrudent aussitôt. Options : échelle ou largeur cible, centre, rotation, **bloc rigide** (le dessin ne se
  déforme plus, il garde 2 degrés de liberté : on le place en cotant son point central ; supprimer ce point
  libère le bloc).
* **Calque image** (*Image* dans la barre *Esquisse*) : photo ou scan sous l'esquisse, embarqué dans le
  `.silex`, opacité réglable, miroirs H / V. **Calibration en 2 clics** (clic droit > *Calibrate image*) :
  deux points d'une distance connue (règle posée à côté de la pièce, entraxe de deux trous), on tape la
  distance, l'image passe à l'échelle 1:1.
* MCP : `create_emboss_text`, `sketch_import_vector`, `sketch_set_canvas` (calibration
  `{"p1_px", "p2_px", "distance_mm"}`).
* Le format passe en `schema_version: "0.7"` ; les fichiers 0.1 à 0.5 restent lisibles.

## Fiabilité et portabilité

* **Dossier autonome.** `build\bin\Release` contient tout : DLL, plugins Qt, `resources\` (leçons,
  composants, exemples, textes légaux), `mcp_server.py`, `LICENSE`. On peut le copier n'importe où.
  Les ressources sont cherchées dans `SILEX_RESOURCE_DIR`, puis dans `<dossier de silex.exe>\resources`,
  puis dans la copie embarquée dans l'exécutable.
* **Diagnostic d'installation** : `silex --doctor` affiche `[OK] / [WARN] / [FAIL]` pour les ressources,
  OpenCascade, PlaneGCS, les dossiers utilisateur, le serveur IPC et Python ; code de sortie 0 si tout va bien.
  `silex.exe` est une application fenêtrée : dans PowerShell, lance-le avec un tube pour attendre la fin et
  récupérer le code de sortie, par exemple `.\silex.exe --doctor | Out-Host`.
* **Sauvegarde automatique** : toutes les 2 minutes (menu `?` > *Autosave* : 1 à 15 min, ou désactivée), si le
  document a des modifications non enregistrées, dans `%LOCALAPPDATA%\Silex\recovery\*.autosave.json`.
  Le fichier disparaît à l'enregistrement et à la fermeture normale. Au démarrage suivant un plantage,
  Silex propose **Restaurer**, **Ignorer** ou **Supprimer**. `silex --recover <fichier>` restaure sans
  dialogue ; `--no-recovery` ou `SILEX_NO_RECOVERY=1` désactivent la question.
* **Test de relocalisation** (`ctest -R RelocationTest`, script `tools\relocation_test.ps1`) : copie
  `bin\Release` seul dans `C:\Temp\Silex_Portable_Test`, lance `silex --doctor`, les tests qui lisent des
  ressources (avec `SILEX_PORTABLE_ONLY=1`, qui interdit tout repli sur le dépôt source) et
  `mcp_server.py --check`.

## Licence

Silex est distribué sous **licence MIT** (fichier `LICENSE`). Les bibliothèques embarquées gardent leur
licence : OpenCascade (LGPL-2.1 + exception OCCT), Qt 6 (LGPL-3), PlaneGCS / FreeCAD (LGPL-2.1+, lié
statiquement, sources complètes dans `third_party/planegcs`), nlohmann/json (MIT), etc. La liste complète
et les textes sont dans `THIRD-PARTY-NOTICES.txt`, régénéré par `python tools/legal/build_notices.py`.
Ils sont aussi affichés par le menu `?` > *About Silex / Licenses* (avec la version et le commit Git).

## Écarts assumés par rapport aux guidelines

* **UI en Qt Widgets plutôt qu'en QML.** C'est plus rapide à faire évoluer pour des panneaux
  d'inspecteur dynamiques ; le viewport reste sur QRhi, ce qui permettra un passage à QML plus tard.
* **Picking par BVH sur le CPU plutôt que par Color-ID sur le GPU.** Les deux options sont autorisées par
  les guidelines. Le BVH évite une relecture GPU synchrone.
* **Phases 4 et 5 non couvertes**, hormis le mode sombre et le menu radial, avancés car peu coûteux :
  gizmos 3D génériques, API Python.

Licence : voir la section « Licence » ci-dessus.
