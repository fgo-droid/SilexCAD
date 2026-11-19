# Silex — CAO 3D paramétrique open-source

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
| Modèle | `S` esquisse · `E` extrusion · `R` révolution · `F` congé · `C` chanfrein · `W`/`Home` cadrer · `0 1 3 7` vues |
| Esquisse | `L` ligne · `R` rectangle · `C` cercle · `A` arc · `D` cote · `X` construction · `H` `V` `I` `P` `K` `T` `Q` `F` contraintes · `Suppr` · `Entrée` terminer |
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
quitte), `fichier.silex`.

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

Installation :

```powershell
python -m pip install --user -r tools\mcp_bridge\requirements.txt
```

Ensuite, déclare le serveur dans le client MCP en t'inspirant de `tools/mcp_bridge/mcp_config.example.json`.
Le test `Ipc.AgentBuildsDrilledFilletedBox` couvre la chaîne de bout en bout : il construit un boîtier
percé avec congés via le pipe, puis vérifie la validité, le volume, la survie TNP, l'annulation, la
capture et l'export STEP/STL.

## Écarts assumés par rapport aux guidelines

* **UI en Qt Widgets plutôt qu'en QML.** C'est plus rapide à faire évoluer pour des panneaux
  d'inspecteur dynamiques ; le viewport reste sur QRhi, ce qui permettra un passage à QML plus tard.
* **Picking par BVH sur le CPU plutôt que par Color-ID sur le GPU.** Les deux options sont autorisées par
  les guidelines. Le BVH évite une relecture GPU synchrone.
* **Phases 4 et 5 non couvertes**, hormis le mode sombre et le menu radial, avancés car peu coûteux :
  gizmos 3D génériques, API Python.

## Licence

Silex est distribué sous **GNU LGPL version 2.1 ou ultérieure** (`LGPL-2.1-or-later`), dont le texte figure
dans [`LICENSE`](LICENSE).

```
Silex — CAO 3D paramétrique
Copyright (C) 2026 les contributeurs de Silex

Ce programme est un logiciel libre ; vous pouvez le redistribuer et/ou le modifier selon les termes de la
GNU Lesser General Public License telle que publiée par la Free Software Foundation, version 2.1 de la
licence ou (à votre choix) toute version ultérieure. Il est distribué dans l'espoir qu'il sera utile, mais
SANS AUCUNE GARANTIE ; voir la licence pour plus de détails.
```

Composants tiers :

| Composant | Licence | Utilisation |
| --- | --- | --- |
| OpenCascade (OCCT) | LGPL-2.1, avec l'exception Open CASCADE | DLL, via vcpkg |
| PlaneGCS (extrait de FreeCAD 1.0) | LGPL-2.1-or-later | sources dans `third_party/planegcs` (voir son README) |
| Qt 6 | LGPL-3.0 | DLL, via vcpkg |
| Eigen, Boost, nlohmann/json, GoogleTest | MPL-2.0, BSL-1.0, MIT, BSD-3 | via vcpkg |

Les fichiers téléchargés ou produits (dossier `exports/`), comme le modèle 3D de la carte LilyGO, ne font
pas partie du dépôt.
