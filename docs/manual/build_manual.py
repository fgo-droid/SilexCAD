"""Generates the Silex user manual (French, plain-language) as a Word document.

    python docs/manual/build_manual.py

Inputs : docs/manual/img/*.png (captures made through the MCP server and `silex --screenshot`).
Output : docs/manual/Silex_Manuel_utilisateur.docx
"""
from pathlib import Path

from docx import Document
from docx.enum.section import WD_SECTION
from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_BREAK
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Cm, Pt, RGBColor
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
IMG = HERE / "img"
OUT = HERE / "Silex_Manuel_utilisateur.docx"

ACCENT = RGBColor(0x1F, 0x6F, 0xB8)
DARK = RGBColor(0x22, 0x2A, 0x35)
GREY = RGBColor(0x5F, 0x67, 0x73)
TIP_BG, WARN_BG, HEAD_BG = "E8F2FC", "FFF3E0", "1F6FB8"


# ----------------------------------------------------------------------------- image prep

def annotate_overview():
    """Numbered call-outs on the main window capture."""
    src, dst = IMG / "ui_overview.png", IMG / "ui_overview_annotated.png"
    im = Image.open(src).convert("RGB")
    d = ImageDraw.Draw(im)
    try:
        font = ImageFont.truetype("arialbd.ttf", 26)
    except OSError:
        font = ImageFont.load_default()
    marks = {1: (100, 74), 2: (640, 74), 3: (250, 300), 4: (1330, 420), 5: (430, 805),
             6: (770, 860), 7: (110, 760), 8: (985, 905), 9: (960, 140), 10: (318, 74)}
    for n, (x, y) in marks.items():
        r = 21
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 152, 0), outline=(255, 255, 255), width=3)
        tw = d.textlength(str(n), font=font)
        d.text((x - tw / 2, y - 15), str(n), fill=(255, 255, 255), font=font)
    im.save(dst)
    return dst


# ----------------------------------------------------------------------------- docx helpers

def shade(cell, hex_color):
    tc_pr = cell._tc.get_or_add_tcPr()
    shd = OxmlElement("w:shd")
    shd.set(qn("w:val"), "clear")
    shd.set(qn("w:color"), "auto")
    shd.set(qn("w:fill"), hex_color)
    tc_pr.append(shd)


def keep_rows_together(table):
    """A row never splits across two pages (boxes and table rows stay readable)."""
    for row in table.rows:
        tr_pr = row._tr.get_or_add_trPr()
        cant = OxmlElement("w:cantSplit")
        cant.set(qn("w:val"), "true")
        tr_pr.append(cant)


def set_cell_margins(table, cm=0.18):
    tbl_pr = table._tbl.tblPr
    mar = OxmlElement("w:tblCellMar")
    for side in ("top", "left", "bottom", "right"):
        el = OxmlElement(f"w:{side}")
        el.set(qn("w:w"), str(int(cm * 567)))
        el.set(qn("w:type"), "dxa")
        mar.append(el)
    tbl_pr.append(mar)


def add_runs(paragraph, text):
    """Very small markup: **bold** and `key` segments."""
    import re
    for part in re.split(r"(\*\*[^*]+\*\*|`[^`]+`)", text):
        if not part:
            continue
        if part.startswith("**"):
            r = paragraph.add_run(part[2:-2])
            r.bold = True
        elif part.startswith("`"):
            r = paragraph.add_run(part[1:-1])
            r.font.name = "Consolas"
            r.font.size = Pt(9.5)
            r.font.color.rgb = RGBColor(0x9C, 0x2B, 0x2B)
        else:
            paragraph.add_run(part)
    return paragraph


class Manual:
    def __init__(self):
        self.doc = Document()
        self.fig = 0
        sec = self.doc.sections[0]
        sec.page_height, sec.page_width = Cm(29.7), Cm(21.0)
        for side in ("left_margin", "right_margin"):
            setattr(sec, side, Cm(2.0))
        sec.top_margin, sec.bottom_margin = Cm(2.0), Cm(2.0)
        st = self.doc.styles
        st["Normal"].font.name = "Calibri"
        st["Normal"].font.size = Pt(11)
        st["Normal"].paragraph_format.space_after = Pt(6)
        st["Normal"].paragraph_format.line_spacing = 1.12
        for lvl, size in ((1, 20), (2, 15), (3, 12.5)):
            h = st[f"Heading {lvl}"]
            h.font.name = "Calibri"
            h.font.size = Pt(size)
            h.font.bold = True
            h.font.color.rgb = ACCENT if lvl < 3 else DARK
            h.paragraph_format.space_before = Pt(18 if lvl == 1 else 12)
            h.paragraph_format.space_after = Pt(6)
            h.paragraph_format.keep_with_next = True
            # East-Asian font fallback in the theme would override "Calibri" otherwise
            h.element.rPr.rFonts.set(qn("w:asciiTheme"), "minorHAnsi") if h.element.rPr is not None else None
        self._footer()

    def _footer(self):
        p = self.doc.sections[0].footer.paragraphs[0]
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        r = p.add_run("Silex — Manuel utilisateur  ·  page ")
        r.font.size, r.font.color.rgb = Pt(8.5), GREY
        for code in ("begin", None, "end"):
            if code is None:
                instr = OxmlElement("w:instrText")
                instr.set(qn("xml:space"), "preserve")
                instr.text = "PAGE"
                run = p.add_run()
                run.font.size = Pt(8.5)
                run._r.append(instr)
            else:
                fld = OxmlElement("w:fldChar")
                fld.set(qn("w:fldCharType"), code)
                run = p.add_run()
                run.font.size = Pt(8.5)
                run._r.append(fld)

    # -- blocks
    def h1(self, t): self.doc.add_heading(t, 1)
    def h2(self, t): self.doc.add_heading(t, 2)
    def h3(self, t): self.doc.add_heading(t, 3)

    def p(self, text, italic=False, size=None, color=None, align=None):
        para = add_runs(self.doc.add_paragraph(), text)
        for r in para.runs:
            if italic:
                r.italic = True
            if size:
                r.font.size = Pt(size)
            if color:
                r.font.color.rgb = color
        if align:
            para.alignment = align
        return para

    def bullets(self, items):
        for it in items:
            para = self.doc.add_paragraph(style="List Bullet")
            add_runs(para, it)
            para.paragraph_format.space_after = Pt(3)

    def steps(self, items):
        """Manually numbered actions (restart at 1 for every list)."""
        for i, it in enumerate(items, 1):
            para = self.doc.add_paragraph()
            para.paragraph_format.left_indent = Cm(0.9)
            para.paragraph_format.first_line_indent = Cm(-0.6)
            para.paragraph_format.space_after = Pt(3)
            r = para.add_run(f"{i}.  ")
            r.bold = True
            r.font.color.rgb = ACCENT
            add_runs(para, it)

    def box(self, kind, text):
        label, bg = {"tip": ("💡 Astuce", TIP_BG), "warn": ("⚠️ Attention", WARN_BG),
                     "eli": ("🧠 En clair", "EEF7EE")}[kind]
        t = self.doc.add_table(rows=1, cols=1)
        t.alignment = WD_TABLE_ALIGNMENT.CENTER
        set_cell_margins(t, 0.25)
        c = t.cell(0, 0)
        shade(c, bg)
        c.width = Cm(17)
        para = c.paragraphs[0]
        r = para.add_run(label + " — ")
        r.bold = True
        add_runs(para, text)
        keep_rows_together(t)
        self.doc.add_paragraph().paragraph_format.space_after = Pt(2)

    def table(self, header, rows, widths):
        t = self.doc.add_table(rows=1, cols=len(header))
        t.style = "Table Grid"
        t.alignment = WD_TABLE_ALIGNMENT.CENTER
        set_cell_margins(t, 0.12)
        for i, h in enumerate(header):
            c = t.rows[0].cells[i]
            shade(c, HEAD_BG)
            c.width = Cm(widths[i])
            r = c.paragraphs[0].add_run(h)
            r.bold = True
            r.font.color.rgb = RGBColor(0xFF, 0xFF, 0xFF)
            r.font.size = Pt(10)
        for k, row in enumerate(rows):
            cells = t.add_row().cells
            for i, v in enumerate(row):
                cells[i].width = Cm(widths[i])
                if k % 2:
                    shade(cells[i], "F3F6FA")
                para = add_runs(cells[i].paragraphs[0], v)
                for r in para.runs:
                    r.font.size = Pt(10)
        keep_rows_together(t)
        self.doc.add_paragraph().paragraph_format.space_after = Pt(2)

    def image(self, path, caption, width_cm=16.5):
        self.fig += 1
        para = self.doc.add_paragraph()
        para.alignment = WD_ALIGN_PARAGRAPH.CENTER
        para.paragraph_format.keep_with_next = True
        para.add_run().add_picture(str(path), width=Cm(width_cm))
        cap = self.doc.add_paragraph()
        cap.alignment = WD_ALIGN_PARAGRAPH.CENTER
        r = cap.add_run(f"Figure {self.fig} — {caption}")
        r.italic, r.font.size, r.font.color.rgb = True, Pt(9), GREY

    def page_break(self):
        self.doc.add_paragraph().add_run().add_break(WD_BREAK.PAGE)

    def toc(self):
        para = self.doc.add_paragraph()
        run = para.add_run()
        for kind, text in (("begin", None), (None, 'TOC \\o "1-2" \\h \\z \\u'), ("separate", None),
                           (None, None), ("end", None)):
            if kind:
                f = OxmlElement("w:fldChar")
                f.set(qn("w:fldCharType"), kind)
                run._r.append(f)
            elif text:
                instr = OxmlElement("w:instrText")
                instr.set(qn("xml:space"), "preserve")
                instr.text = text
                run._r.append(instr)
            else:
                t = OxmlElement("w:t")
                t.text = "Clic droit › Mettre à jour les champs pour afficher la table des matières."
                run._r.append(t)


# ----------------------------------------------------------------------------- content

def build():
    m = Manual()
    overview = annotate_overview()
    d = m.doc

    # ---------------------------------------------------------------- cover
    for _ in range(6):
        d.add_paragraph()
    t = m.p("Silex", size=48, color=ACCENT, align=WD_ALIGN_PARAGRAPH.CENTER)
    t.runs[0].bold = True
    m.p("Manuel utilisateur", size=24, color=DARK, align=WD_ALIGN_PARAGRAPH.CENTER)
    m.p("La CAO 3D paramétrique, expliquée simplement", size=14, color=GREY, align=WD_ALIGN_PARAGRAPH.CENTER,
        italic=True)
    d.add_paragraph()
    para = d.add_paragraph()
    para.alignment = WD_ALIGN_PARAGRAPH.CENTER
    para.add_run().add_picture(str(IMG / "step_08.png"), width=Cm(13))
    d.add_paragraph()
    m.p("Version 0.7  ·  Novembre 2026", size=11, color=GREY, align=WD_ALIGN_PARAGRAPH.CENTER)
    m.page_break()

    title = m.p("Sommaire", size=20, color=ACCENT)  # not a heading: it must not list itself
    title.runs[0].bold = True
    m.toc()

    # ---------------------------------------------------------------- 1. intro
    m.h1("1. Avant de commencer")
    m.h2("1.1 Silex, c'est quoi ?")
    m.p("Silex est un logiciel de **CAO** (Conception Assistée par Ordinateur) : il sert à dessiner des "
        "pièces en 3D, par exemple pour les imprimer en 3D, les usiner ou les envoyer à un fabricant.")
    m.p("Silex est **paramétrique**. Cela veut dire qu'il ne retient pas seulement la forme finale de ta "
        "pièce, mais la **recette** qui l'a fabriquée : « dessine un rectangle de 40 × 85, monte-le de 10 mm, "
        "perce un trou de Ø20… ». Si tu changes un ingrédient (le diamètre du trou, par exemple), Silex "
        "refait toute la recette et la pièce se met à jour toute seule.")
    m.box("eli", "Imagine une recette de gâteau plutôt qu'une photo du gâteau. Avec la photo, tu ne peux rien "
                 "changer. Avec la recette, tu remplaces « 2 œufs » par « 3 œufs » et tu obtiens un nouveau gâteau, "
                 "sans tout recommencer.")
    m.h2("1.2 Les six mots à connaître")
    m.table(["Mot", "Ce que ça veut dire"], [
        ["**Esquisse**", "Un dessin 2D (lignes, cercles, arcs) posé sur un plan. C'est le point de départ de presque tout."],
        ["**Contrainte**", "Une règle sur l'esquisse : « cette ligne est horizontale », « ces deux points se touchent »."],
        ["**Cote**", "Une contrainte avec un nombre : « cette ligne mesure 40 mm », « ce cercle fait Ø20 »."],
        ["**Fonction**", "Une étape de la recette : extrusion, congé, chanfrein, perçage… Chacune apparaît dans la timeline."],
        ["**Corps**", "Un solide 3D. Un projet peut contenir plusieurs corps (par exemple une base et un couvercle)."],
        ["**Timeline**", "La barre en bas de l'écran qui montre toutes les étapes, dans l'ordre. C'est ta recette."],
    ], [3.2, 13.8])
    m.h2("1.3 Installer et lancer Silex")
    m.p("Silex est **portable** : pas d'installateur, il suffit de copier son dossier.")
    m.steps([
        "Copie le dossier `bin\\Release` (environ 120 Mo) où tu veux, par exemple `C:\\Silex` ou une clé USB. "
        "Il contient tout : le programme, les leçons, les exemples, les composants, la police du texte, les licences "
        "et la passerelle IA `mcp_server.py`. Aucun chemin n'est figé : le dossier marche partout.",
        "Double-clique sur `silex.exe`.",
        "Si Windows affiche une erreur à propos de `MSVCP140.dll` ou `VCRUNTIME140.dll`, installe le "
        "« Visual C++ Redistributable x64 » de Microsoft (commande : `winget install Microsoft.VCRedist.2015+.x64`).",
    ])
    m.p("Configuration nécessaire : Windows 10 ou 11 en 64 bits et une carte graphique compatible Direct3D 11 "
        "(c'est le cas de presque tous les PC récents).")
    m.box("tip", "Pour ouvrir directement un fichier : `silex.exe monprojet.silex`. Pour voir une pièce de "
                 "démonstration : `silex.exe --demo`. Pour vérifier que l'installation est complète : "
                 "`silex.exe --doctor` (voir § 4.40).")

    # ---------------------------------------------------------------- 2. basics
    m.h1("2. Prise en main : les fonctions de base")
    m.h2("2.1 L'écran de Silex")
    m.image(overview, "La fenêtre principale (pièce du tutoriel du chapitre 3 chargée)")
    m.table(["#", "Zone", "À quoi ça sert"], [
        ["1", "Barre **Fichier**", "Nouveau, Ouvrir, Enregistrer, Importer STEP, Exporter (STEP, STL, 3MF), Annuler, Rétablir."],
        ["2", "Barre **Modèle**", "Les outils : Sketch (esquisse), Create ▾ (boîte, cylindre, perçage, texte…), Extrude, Press/Pull, Revolve, Sweep, Loft, Modify ▾ (congé, chanfrein, coque…), Pattern ▾ (symétrie, répétitions), Combine, Thread, Construct ▾, Inspect ▾, Drawing, Parameters, Fit, Learn. Tout à droite : la bascule Essentiel / Complet (§ 4.34)."],
        ["3", "**Vue 3D**", "Là où tu vois et sélectionnes ta pièce. La grille représente le sol (plan XY)."],
        ["4", "**Inspecteur**", "Le panneau de droite : il montre les réglages de l'outil en cours, ou la liste des corps quand aucun outil n'est actif."],
        ["5", "**Timeline**", "Toutes les étapes de ta pièce, de gauche à droite. Double-clic sur une étape pour la modifier."],
        ["6", "**Marqueur de retour**", "La barre orange : tout ce qui est à sa droite est temporairement désactivé. Glisse-la pour « remonter le temps »."],
        ["7", "**Trièdre**", "Montre l'orientation : X rouge, Y vert, Z bleu (Z = vers le haut)."],
        ["8", "**Barre d'état**", "Pastille MCP, nombre de corps et de triangles, temps de calcul, grille magnétique (Snap), plateau d'impression (Bed), et le nombre d'erreurs ou d'avertissements s'il y en a."],
        ["9", "**ViewCube**", "Clique une face, une arête ou un coin pour changer de vue ; la maison revient à la vue d'ensemble (§ 4.24)."],
        ["10", "Menu **?**", "À propos de Silex et licences, réglage de la sauvegarde automatique, dossier de récupération (§ 4.39 et 4.41)."],
    ], [0.8, 3.4, 12.8])
    m.box("tip", "Silex n'a pas d'« ateliers » à changer : les outils s'adaptent tout seuls à ce que tu fais. "
                 "Par exemple, la barre d'esquisse (Line, Rectangle, Circle…) n'apparaît que lorsque tu dessines.")

    m.h2("2.2 Se déplacer en 3D")
    m.table(["Pour…", "Fais…"], [
        ["Zoomer", "Molette de la souris (le zoom se fait vers le curseur)."],
        ["Déplacer la vue (panoramique)", "Clic **milieu** maintenu, ou `Alt` + `Maj` + clic gauche."],
        ["Tourner autour de la pièce (orbite)", "`Maj` + clic **milieu** maintenu, ou `Alt` + clic gauche."],
        ["Tout voir (cadrer)", "Touche `W` ou `Origine`, ou double-clic milieu, ou bouton Fit."],
        ["Vues standard", "`1` face, `3` droite, `7` dessus, `0` isométrique."],
    ], [5.5, 11.5])

    m.h2("2.3 Sélectionner")
    m.bullets([
        "**Clic gauche** sur une face ou une arête : elle devient bleue (sélectionnée).",
        "`Ctrl` + clic : ajoute ou retire un élément de la sélection.",
        "**Double-clic** sur une pièce : sélectionne la **pièce entière**. `Ctrl` + double-clic pour en ajouter d'autres.",
        "`Échap` : vide la sélection.",
        "En survolant un élément, son **nom** s'affiche en bas à droite de la vue (voir § 4.10, utile pour comprendre les références).",
    ])

    m.h2("2.4 Dessiner une esquisse")
    m.p("Toute pièce commence par une esquisse : un dessin 2D posé sur un plan.")
    m.steps([
        "Appuie sur `S` (ou clique sur **Sketch**).",
        "Trois carrés colorés apparaissent : ce sont les plans **XY** (sol), **XZ** (face avant) et **YZ** (côté). "
        "Clique sur l'un d'eux, ou sur une **face plane** de ta pièce.",
        "La caméra se place face au plan. Dessine avec les outils : `L` ligne, `R` rectangle, `C` cercle, `A` arc.",
        "Ajoute des cotes avec `D` (voir § 2.5).",
        "Termine avec `Entrée` ou **Finish sketch**.",
    ])
    m.image(IMG / "ui_sketch.png", "Éditeur d'esquisse : rectangle entièrement contraint (blanc), cotes 85, 40 et 20, panneau des outils à droite")
    m.p("Pendant que tu dessines, Silex t'aide en ajoutant tout seul des **contraintes automatiques** :")
    m.bullets([
        "**Accrochage** : près d'un point existant, un carré jaune apparaît. Clique et les deux points seront collés (coïncidents).",
        "**Horizontal / vertical** : si ta ligne est presque horizontale, un petit « H » apparaît et elle devient parfaitement horizontale.",
        "**Tangence** : une ligne qui part de la fin d'un arc dans son prolongement devient tangente à l'arc.",
    ])
    m.table(["Outil", "Touche", "Comment l'utiliser"], [
        ["Line (ligne)", "`L`", "Clic sur le point de départ, puis un clic par point suivant (polyligne). `Échap` pour arrêter."],
        ["Rectangle", "`R`", "Clic sur un coin, clic sur le coin opposé."],
        ["Circle (cercle)", "`C`", "Clic au centre, clic pour fixer le rayon."],
        ["Arc", "`A`", "Clic au centre, clic au début, clic à la fin (sens inverse des aiguilles d'une montre)."],
        ["Construction", "`X`", "Transforme la courbe sélectionnée en trait de construction (pointillés orange) : elle guide le dessin mais ne fait pas partie de la forme."],
        ["Supprimer", "`Suppr`", "Efface la courbe ou la contrainte sélectionnée."],
    ], [3.2, 1.6, 12.2])

    m.h2("2.5 Contraintes et cotes : rendre l'esquisse « solide »")
    m.p("Une esquisse sans cotes, c'est comme une maquette en élastique : on peut la tirer dans tous les sens. "
        "Les cotes et contraintes la rendent rigide. La **couleur** te dit où tu en es :")
    m.table(["Couleur", "Signification"], [
        ["**Bleu**", "Sous-contraint : il reste de la liberté (tu peux encore tirer un point à la souris). Le nombre de « degrés de liberté » (DOF) s'affiche en haut à gauche."],
        ["**Blanc**", "Totalement contraint : tout est défini, rien ne peut bouger par accident. C'est l'objectif."],
        ["**Rouge**", "Sur-contraint : deux règles se contredisent. Silex refuse d'ailleurs d'ajouter une contrainte qui rendrait l'esquisse rouge."],
        ["Orange pointillé", "Géométrie de construction."],
    ], [3.2, 13.8])
    m.p("**Ajouter une cote** : appuie sur `D`, puis :")
    m.bullets([
        "clique une **ligne** puis dans le vide → longueur de la ligne ;",
        "clique une ligne puis une **autre ligne** → angle entre les deux ;",
        "clique un **cercle** → diamètre ; un **arc** → rayon ;",
        "clique un **point** puis un autre point (ou une ligne) → distance.",
    ])
    m.p("Une petite case s'ouvre : tape la valeur et appuie sur `Entrée`. Pour modifier une cote plus tard, "
        "**double-clique** dessus.")
    m.table(["Contrainte", "Touche", "Effet (sélectionne d'abord les éléments)"], [
        ["Horizontale / Verticale", "`H` / `V`", "La ligne devient horizontale / verticale."],
        ["Coïncidence", "`I`", "Deux points se touchent (ou un point sur une courbe)."],
        ["Parallèle / Perpendiculaire", "`P` / `K`", "Deux lignes parallèles / à angle droit."],
        ["Tangente", "`Maj+T`", "Une ligne et un cercle (ou deux cercles) se touchent sans faire d'angle."],
        ["Égalité", "`Q`", "Deux lignes de même longueur, ou deux cercles de même rayon."],
        ["Blocage", "`F`", "Fige un point ou une courbe à sa position actuelle."],
    ], [4.8, 2.2, 10.0])
    m.box("tip", "Fais glisser un point **bleu** avec la souris : tu vois tout de suite ce qui n'est pas encore "
                 "défini. C'est la façon la plus rapide de trouver la cote qui manque.")

    m.h2("2.6 Extruder : passer de la 2D à la 3D")
    m.p("L'extrusion « tire » une forme 2D fermée pour en faire un volume, comme on pousse de la pâte à "
        "travers un emporte-pièce.")
    m.steps([
        "Appuie sur `E` (ou **Extrude**). Si tu sors d'une esquisse qui n'a qu'une forme fermée, elle est choisie toute seule. "
        "Sinon, clique sur les **profils** (les zones fermées, en bleu clair).",
        "Règle la **distance** : tape-la dans l'inspecteur, ou **tire la flèche orange** dans la vue (`Ctrl` pour avancer au millimètre).",
        "Choisis la **direction** : d'un côté, ou symétrique ; le bouton **Flip direction** inverse le sens.",
        "Choisis l'**opération** (voir tableau ci-dessous), puis **OK** ou `Entrée`.",
    ])
    m.image(IMG / "ui_extrude.png", "Édition d'une extrusion : aperçu transparent, flèche de distance et panneau de réglages")
    m.table(["Opération", "Couleur de l'aperçu", "Résultat"], [
        ["New body (nouveau corps)", "Vert", "Crée une pièce séparée."],
        ["Join (joindre)", "Bleu", "Ajoute de la matière à la pièce existante."],
        ["Cut (couper)", "Rouge", "Enlève de la matière : c'est ainsi qu'on fait des trous et des poches."],
        ["Intersect", "Bleu", "Ne garde que la partie commune."],
    ], [4.6, 3.6, 8.8])
    m.box("tip", "Raccourci : **double-clique sur un profil** dans la vue pour lancer directement l'extrusion.")

    m.h2("2.7 Congés et chanfreins : adoucir les arêtes")
    m.p("Un **congé** arrondit une arête (comme le bord d'un téléphone). Un **chanfrein** la coupe en biais "
        "(comme le bord d'une table).")
    m.steps([
        "Appuie sur `F` (congé) ou `C` (chanfrein).",
        "Clique les arêtes à traiter (re-clique pour retirer une arête).",
        "Règle le rayon (ou la distance) dans l'inspecteur : l'aperçu se met à jour en direct.",
        "Valide avec **OK**.",
    ])
    m.image(IMG / "ui_fillet.png", "Édition des congés R10 sur le haut du montant")
    m.box("warn", "Un rayon trop grand pour la pièce ne peut pas être calculé. Silex ne plante pas : la "
                  "fonction est soulignée en **rouge** dans la timeline, le message d'erreur s'affiche, et la "
                  "pièce garde sa dernière forme valide. Il suffit de réduire le rayon.")

    m.h2("2.8 La timeline : modifier sans tout recommencer")
    m.bullets([
        "**Double-clic** sur une étape : tu la rouvres avec tous ses réglages. Les étapes suivantes se recalculent quand tu valides.",
        "**Glisse le marqueur orange** vers la gauche pour revoir la pièce telle qu'elle était à une étape précise. Une nouvelle fonction "
        "s'insère à l'endroit du marqueur.",
        "**Clic droit** sur une étape : Modifier, Renommer, Désactiver (suppress), Placer le marqueur ici, Supprimer.",
        "**Glisse une étape** pour changer l'ordre (refusé si une autre étape en dépend).",
        "Souligné **rouge** = erreur, **orange** = avertissement. Survole l'icône pour lire le message.",
        "`Ctrl+Z` / `Ctrl+Y` : annuler / rétablir.",
    ])

    m.h2("2.9 Enregistrer, ouvrir, exporter")
    m.table(["Action", "Raccourci", "Fichier"], [
        ["Enregistrer / Enregistrer sous", "`Ctrl+S` / `Ctrl+Maj+S`", "`.silex` : ton projet complet et modifiable (format texte JSON, lisible et versionnable). Silex 0.7 enregistre au format 0.7 (texte, images de référence…) et ouvre sans problème les fichiers des versions précédentes."],
        ["Ouvrir", "`Ctrl+O`", "`.silex` uniquement."],
        ["Importer un STEP", "`Ctrl+I`", "`.step` / `.stp` venant d'un autre logiciel ou d'un fabricant (pièce sans historique)."],
        ["Exporter STEP", "`Ctrl+E`", "Format d'échange universel pour les autres logiciels de CAO."],
        ["Exporter STL", "`Ctrl+Maj+E`", "Maillage pour l'impression 3D. Silex demande la précision (0,05 mm par défaut)."],
        ["Exporter 3MF", "`Ctrl+3`", "Toutes les pièces dans un seul fichier pour le trancheur, avec noms et couleurs (§ 4.21)."],
    ], [4.6, 3.6, 8.8])
    m.box("warn", "Un fichier STEP ou STL ne contient que la forme, pas la recette : pour pouvoir modifier ta pièce "
                  "plus tard, garde toujours le fichier **.silex**.")
    m.box("tip", "Silex fait une **sauvegarde automatique** toutes les 2 minutes tant que tu as des modifications non "
                 "enregistrées. Après un plantage ou une coupure de courant, il te propose de récupérer ton travail au "
                 "démarrage suivant (§ 4.39).")
    m.page_break()

    # ---------------------------------------------------------------- 3. tutorial
    m.h1("3. Tutoriel pas à pas : l'équerre de fixation")
    m.p("Dans ce tutoriel, tu vas dessiner l'équerre ci-dessous en utilisant les fonctions les plus courantes : "
        "esquisse, cotes, extrusion (nouveau corps, joindre, couper), esquisse sur une face, chanfrein et congé. "
        "Compte environ 20 minutes.")
    m.image(IMG / "plan_reference.png", "Le plan de référence de la pièce", width_cm=12)
    m.p("Interprétation retenue (en mm) : semelle 40 × 85 × 10, rail avant 70 × 20 × 15 chanfreiné à 5 mm, "
        "montant 40 × 20 × 65, renfort 40 × 10 jusqu'à 25 de haut, perçage Ø20 dans la semelle, deux trous Ø10 "
        "à 20 d'entraxe dans la tête, arrondis R10 en haut du montant. Repère : X vers la droite, Y vers l'arrière, "
        "Z vers le haut, origine au milieu de l'arête avant de la semelle.", italic=True, color=GREY)

    steps = [
        ("Étape 1 — La semelle",
         "Créer la plaque de base de 40 × 85 mm et 10 mm d'épaisseur.",
         ["Appuie sur `S` et clique sur le plan **XY** (le carré bleu, au sol).",
          "Appuie sur `R` et trace un rectangle à peu près centré sur l'axe vertical, en partant de l'axe X.",
          "Appuie sur `D` : clique l'arête du bas puis dans le vide, tape `40`. Recommence avec l'arête de gauche : `85`.",
          "Place l'esquisse : cote `D` entre l'origine et le coin bas-gauche (20 en X) et contrainte `I` du coin sur l'axe X. "
          "L'esquisse devient **blanche**.",
          "Appuie sur `E`, tape `10`, opération **New body**, puis **OK**."],
         "La semelle : 34 000 mm³ (40 × 85 × 10)."),
        ("Étape 2 — Le rail avant",
         "Ajouter le rail de 70 × 20 mm, haut de 15 mm, à l'avant.",
         ["`S`, plan **XY**.",
          "`R` : rectangle de 70 × 20 posé sur l'arête avant, centré (de X = −35 à X = +35).",
          "`E`, distance `15`, opération **Join** : le rail se soude à la semelle."],
         "Volume : 47 000 mm³."),
        ("Étape 3 — Le chanfrein du rail",
         "Casser l'arête avant-haut du rail.",
         ["Appuie sur `C` (chanfrein).",
          "Clique l'arête **avant-haut** du rail (la longue arête du dessus, côté avant).",
          "Distance `5`, puis **OK**."],
         "Le chanfrein enlève 875 mm³ (5 × 5 ÷ 2 × 70)."),
        ("Étape 4 — Le montant",
         "Monter la partie verticale de 65 mm à l'arrière.",
         ["`S`, plan **XY**.",
          "`R` : rectangle 40 × 20 à l'arrière de la semelle (Y de 65 à 85).",
          "`E`, distance `65`, **Join**. Tire la flèche orange pour voir l'effet, puis tape la valeur exacte."],
         "Volume : 90 125 mm³."),
        ("Étape 5 — Le renfort (esquisse sur une face)",
         "Ajouter la marche de 25 mm devant le montant, en dessinant directement sur la semelle.",
         ["Clique sur la **face supérieure de la semelle** pour la sélectionner, puis appuie sur `S` : l'esquisse se pose sur cette face.",
          "`R` : rectangle 40 × 10 collé contre le montant.",
          "`E`, distance `15`, **Join** (10 de semelle + 15 = 25 de haut)."],
         "Volume : 96 125 mm³."),
        ("Étape 6 — Le perçage Ø20",
         "Percer la semelle de part en part.",
         ["Sélectionne la face supérieure de la semelle, puis `S`.",
          "`C` : cercle au milieu de la semelle. `D` sur le cercle : tape `20`.",
          "`E`, opération **Cut**, distance `10`, puis **Flip direction** pour aller vers le bas (l'aperçu est rouge)."],
         "Le trou enlève 3 141,6 mm³ (π × 10² × 10)."),
        ("Étape 7 — Les deux trous Ø10 de la tête",
         "Percer la tête du montant, de l'avant vers l'arrière.",
         ["Clique la **face avant du montant**, puis `S`.",
          "Deux cercles de Ø10 à 55 mm de haut, écartés de 20 mm (X = −10 et X = +10).",
          "`E`, **Cut**, distance `20` (l'épaisseur du montant), en direction de la matière."],
         "Volume : 89 841,8 mm³."),
        ("Étape 8 — Les arrondis R10",
         "Arrondir le haut du montant.",
         ["Appuie sur `F` (congé).",
          "Clique les **deux arêtes hautes** du montant qui vont de l'avant vers l'arrière.",
          "Rayon `10`, puis **OK**. Les arrondis sont concentriques avec les trous Ø10 : il reste 5 mm de matière tout autour."],
         "La pièce est terminée : 88 983,4 mm³."),
    ]
    for i, (title, goal, actions, result) in enumerate(steps, 1):
        m.h2(title)
        m.p(f"**Objectif :** {goal}")
        m.steps(actions)
        m.image(IMG / f"step_{i:02d}.png", result, width_cm=11.5)
    m.h2("Étape 9 — Vérifier sous un autre angle")
    m.p("Appuie sur `1` pour la vue de face : tu reconnais la silhouette du plan, avec les trous et les arrondis "
        "alignés. `0` ramène la vue isométrique.")
    m.image(IMG / "step_09.png", "Vue de face de l'équerre terminée", width_cm=11.5)
    m.h2("Étape 10 — La magie du paramétrique")
    m.p("Le client veut finalement un perçage de **Ø26** au lieu de Ø20. Pas besoin de tout redessiner :")
    m.steps([
        "Dans la timeline, **double-clique** sur « Esquisse perçage Ø20 ».",
        "**Double-clique** sur la cote 20, tape `26`, `Entrée`, puis **Finish sketch**.",
        "Toute la pièce se recalcule : le trou grandit, et les étapes suivantes (trous Ø10, congés) restent à leur place.",
    ])
    m.image(IMG / "step_10.png", "Le perçage passe à Ø26 : la pièce entière s'est mise à jour (86 815,7 mm³)", width_cm=11.5)
    m.box("eli", "Pourquoi les congés ne se perdent-ils pas quand on modifie une esquisse en amont ? Parce que Silex "
                 "donne à chaque face et à chaque arête un **nom stable** qui décrit son origine (« la face créée en "
                 "extrudant telle ligne »), au lieu d'un simple numéro. Voir § 4.10.")
    m.h2("Pour finir")
    m.steps([
        "`Ctrl+S` : enregistre `equerre.silex` (ton projet modifiable).",
        "`Ctrl+Maj+E` : exporte `equerre.stl` pour l'imprimante 3D.",
        "`Ctrl+E` : exporte `equerre.step` pour un autre logiciel ou un usineur.",
    ])
    m.page_break()

    # ---------------------------------------------------------------- 4. advanced
    m.h1("4. Fonctions avancées")
    m.h2("4.1 Esquisser sur une face")
    m.p("Au lieu des plans XY, XZ ou YZ, tu peux dessiner sur **n'importe quelle face plane** : sélectionne-la "
        "puis appuie sur `S` (ou appuie sur `S` puis clique la face). L'esquisse reste « accrochée » à cette face : "
        "si la pièce change de hauteur, l'esquisse suit.")
    m.h2("4.2 Révolution")
    m.p("La révolution fait tourner un profil autour d'un axe, comme un potier avec son tour. Idéal pour les "
        "pièces rondes : bagues, poulies, entretoises, boutons.")
    m.steps([
        "Dessine le **demi-profil** de la pièce (la moitié de sa coupe), d'un seul côté de l'axe.",
        "Optionnel : trace l'axe avec une ligne de **construction** (`X`).",
        "Appuie sur `R` (hors esquisse), clique le profil, choisis l'**axe** (axe X ou Y de l'esquisse, ou une de tes lignes) et l'**angle** (360° pour un tour complet).",
    ])
    m.box("warn", "Le profil ne doit pas traverser l'axe, sinon la révolution échoue (soulignée en rouge).")
    # ------------------------------------------------ v0.2 tools
    m.h2("4.3 Coque (Shell)")
    m.p("La coque transforme un bloc plein en boîte à parois fines, comme si on creusait un bloc de savon en "
        "gardant une épaisseur régulière. Les faces que tu choisis deviennent les **ouvertures**.")
    m.steps([
        "Appuie sur `H`, ou clique sur **Shell** dans la barre Modèle (ou dans le menu radial).",
        "Clique la ou les faces à ouvrir, par exemple le dessus d'une boîte. Elles apparaissent dans "
        "**Removed Faces**, avec un compteur ; re-clique une face pour la retirer, ou utilise **Clear**.",
        "Règle **Thickness** (2,00 mm par défaut) et **Direction** : **Inside** pour que les parois poussent vers "
        "l'intérieur (la pièce garde sa taille extérieure), **Outside** pour qu'elles poussent vers l'extérieur "
        "(la cavité garde la taille d'origine).",
        "Le résultat apparaît en transparence pendant le réglage. Valide avec **OK**.",
    ])
    m.image(IMG / "v02_ui_shell.png", "Édition d'une coque : face supérieure retirée, épaisseur 2,00 mm vers l'intérieur",
            width_cm=15)
    m.image(IMG / "v02_shell_after.png", "Résultat : une boîte à parois de 2 mm, congés conservés à l'intérieur",
            width_cm=11)
    m.box("tip", "Fais les congés extérieurs **avant** la coque : les parois intérieures suivront les arrondis.")

    m.h2("4.4 Corps ou fonctions : que copie-t-on ?")
    m.p("Les trois outils suivants (**Mirror**, **Linear Pattern**, **Circular Pattern**) copient quelque chose. "
        "Dans l'inspecteur, le menu **Type** te laisse choisir :")
    m.table(["Type", "Ce qui est copié", "Exemple"], [
        ["**Bodies**", "Des pièces entières. Clique-les dans la vue.", "Dupliquer une patte de fixation, faire la deuxième moitié d'une pièce symétrique."],
        ["**Features**", "La matière ajoutée ou enlevée par une **extrusion** ou une **révolution**. Coche-les dans la liste.", "Répéter un perçage, un bossage ou une rainure sur la même pièce."],
    ], [2.6, 7.0, 7.4])
    m.box("eli", "Copier une **fonction**, c'est copier le geste plutôt que le résultat. Si tu répètes « le perçage », "
                 "Silex refait ce perçage à chaque position. Et si tu changes plus tard le diamètre du trou d'origine, "
                 "toutes les copies changent avec lui.")

    m.h2("4.5 Symétrie (Mirror)")
    m.p("La symétrie crée la copie « en miroir » d'une pièce ou d'une fonction par rapport à un plan. C'est idéal "
        "pour ne dessiner que la moitié d'une pièce symétrique.")
    m.steps([
        "Appuie sur `M`, ou clique sur **Mirror**.",
        "Choisis le **Type** (**Bodies** ou **Features**) et désigne les cibles. Si des pièces étaient déjà "
        "sélectionnées, elles sont reprises automatiquement.",
        "Choisis le plan dans **Plane** : **Plane XY**, **Plane XZ** ou **Plane YZ**, ou clique **Pick** puis une "
        "face plane de ta pièce.",
        "Laisse **Join if touching** coché pour que la copie se soude à l'original quand elles se touchent. "
        "Sinon, la copie devient une pièce séparée.",
        "Valide avec **OK**.",
    ])
    m.image(IMG / "v02_ui_mirror.png", "Édition d'une symétrie par rapport au plan YZ, copie soudée à l'original", width_cm=15)
    m.image(IMG / "v02_mirror_before.png", "Avant : la demi-patte, dessinée d'un seul côté du plan YZ", width_cm=8)
    m.image(IMG / "v02_mirror_after.png", "Après Mirror : la patte complète, en une seule pièce", width_cm=8)
    m.box("tip", "Les congés et chanfreins posés **après** la symétrie restent en place si tu modifies ensuite une cote "
                 "de la moitié d'origine : Silex nomme chaque face copiée « miroir de telle face » (voir § 4.10).")

    m.h2("4.6 Répétition linéaire (Linear Pattern)")
    m.p("La répétition linéaire copie une pièce ou une fonction en ligne, ou en grille, à intervalle régulier : "
        "une rangée de trous, des ailettes, des plots…")
    m.steps([
        "Appuie sur `Maj+P`, ou clique sur **Linear Pattern**.",
        "Choisis le **Type** et les cibles (par exemple **Features** et le perçage à répéter).",
        "**Direction 1** : choisis **Axis X**, **Axis Y** ou **Axis Z**, ou clique **Pick** puis une arête droite (sa "
        "direction) ou une face plane (sa normale). Règle **Count** (nombre d'exemplaires, original compris) et "
        "**Spacing** (écart entre deux exemplaires).",
        "**Reverse** inverse le sens. **Symmetric** répartit les copies des deux côtés de l'original.",
        "Pour une grille, remplis **Direction 2 (optional)** avec un **Count** supérieur à 1.",
        "Valide avec **OK**.",
    ])
    m.image(IMG / "v02_ui_linear.png", "Édition d'une grille de 8 × 3 perçages : les copies apparaissent en rouge (enlèvement de matière)",
            width_cm=15)
    m.image(IMG / "v02_linear_after.png", "Résultat : 24 perçages à partir d'un seul trou dessiné", width_cm=11)
    m.box("tip", "L'aperçu reste léger même avec beaucoup d'exemplaires : seules les copies sont affichées en "
                 "transparence pendant le réglage. Le calcul complet n'a lieu qu'une fois.")

    m.h2("4.7 Répétition circulaire (Circular Pattern)")
    m.p("La répétition circulaire copie une pièce ou une fonction autour d'un axe : les trous d'une bride, les "
        "rayons d'une roue, les dents d'un engrenage…")
    m.steps([
        "Appuie sur `Maj+C`, ou clique sur **Circular Pattern**.",
        "Choisis le **Type** et les cibles.",
        "**Axis** : **Axis X**, **Axis Y** ou **Axis Z**, ou **Pick** puis une arête circulaire (son axe) ou une "
        "face cylindrique (par exemple l'alésage central de la bride).",
        "Règle **Count** et **Total angle** (360° pour un tour complet). Avec **Equal spacing**, les exemplaires "
        "sont répartis régulièrement sur l'angle total ; sans, **Total angle** devient l'écart entre deux exemplaires.",
        "**Reverse** tourne dans l'autre sens. Valide avec **OK**.",
    ])
    m.image(IMG / "v02_ui_circular.png", "Édition d'une répétition circulaire de 6 perçages autour de l'axe Z", width_cm=15)
    m.image(IMG / "v02_circular_after.png", "La bride : 6 perçages, puis un chanfrein de 0,6 mm sur chacun", width_cm=8)
    m.image(IMG / "v02_circular_count8.png", "Count passe à 8 : les 6 chanfreins restent sur leurs trous, les 2 nouveaux trous n'en ont pas",
            width_cm=8)
    m.box("eli", "Chaque copie porte un numéro (1, 2, 3…) dans son nom. Quand tu passes de 6 à 8 trous, les trous "
                 "n° 1 à 5 gardent leur nom, même s'ils se déplacent un peu sur le cercle. Les chanfreins posés "
                 "dessus les suivent donc. C'est le même principe qu'au § 4.10.")

    m.h2("4.8 Combiner des pièces : fusion, soustraction, intersection")
    m.p("Quand ton projet contient plusieurs corps, tu peux les combiner après coup.")
    m.steps([
        "**Double-clique** sur la première pièce : c'est la **cible** (elle s'affichera en orange).",
        "`Ctrl` + **double-clic** sur les autres pièces (les **outils**, en bleu). Tu peux aussi cocher les pièces dans l'inspecteur.",
        "**Clic droit** : le menu propose **Fusionner**, **Soustraire de…** et **Intersecter**. Les mêmes boutons sont dans l'inspecteur.",
    ])
    m.p("Pour plus d'options (garder les pièces outils, par exemple), utilise l'outil **Combine** (`B`). Comme tout "
        "le reste, la combinaison apparaît dans la timeline et reste modifiable.")
    m.h2("4.9 Le menu radial")
    m.p("Maintiens le **clic droit**, donne un petit coup de souris vers la commande voulue et relâche : c'est "
        "le **menu radial**. Après quelques jours, ton bras retient les directions et tu ne regardes plus le menu. "
        "Un simple clic droit l'ouvre aussi en mode « clic ». Son contenu change selon le contexte : outils de "
        "modélisation (jusqu'à 12 commandes réparties en cercle), outils d'esquisse, OK / Annuler pendant une édition, "
        "ou fusion quand plusieurs pièces sont sélectionnées.")
    m.h2("4.10 Les références persistantes (le « nommage topologique »)")
    m.p("Quand tu poses un congé sur une arête, Silex doit se souvenir de **quelle** arête il s'agit, même après "
        "une modification en amont. Beaucoup de logiciels numérotent les arêtes (« arête n° 12 ») : si la pièce "
        "change, les numéros changent et le congé saute sur une mauvaise arête.")
    m.p("Silex nomme chaque élément par son **histoire** : par exemple « la face latérale créée par l'extrusion "
        "de telle ligne d'esquisse », et une arête par les deux faces qu'elle sépare. Survole une arête : son nom "
        "s'affiche en bas à droite de la vue. Si un nom disparaît vraiment (tu as supprimé la ligne d'origine), "
        "Silex retrouve l'élément le plus proche par sa forme et sa position, et signale un **avertissement** orange.")
    m.h2("4.11 Importer un STEP et le mesurer")
    m.p("`Ctrl+I` importe une pièce STEP (fournisseur de composants, autre logiciel…). Elle arrive comme un corps "
        "sans historique, mais tu peux la percer, la combiner, lui ajouter des congés, et l'exporter. C'est pratique "
        "pour dessiner un boîtier autour d'une carte électronique : importe le modèle 3D de la carte et place tes "
        "ouvertures en face de ses connecteurs.")
    m.h2("4.12 Piloter Silex avec une IA (MCP)")
    m.p("Silex peut être piloté par un assistant IA comme **Claude Code** ou **Gemini CLI** grâce au protocole "
        "**MCP**. Tu décris la pièce en français, l'assistant appelle les outils de Silex étape par étape, vérifie "
        "la géométrie et exporte les fichiers. Si Silex est ouvert, tu vois la pièce se construire en direct.")
    m.p("**Installation** (une seule fois) :")
    m.steps([
        "Installe Python 3.12 : `winget install --id Python.Python.3.12 -e --scope user`.",
        "Installe la passerelle : `python -m pip install --user -r requirements.txt` (le fichier est à côté de `silex.exe`).",
        "Déclare le serveur dans ton assistant, par exemple Claude Code : `claude mcp add --scope user silex -- python <dossier de Silex>\\mcp_server.py`. "
        "Le fichier `mcp_config.example.json` donne le même réglage pour les autres clients (remplace `<SILEX_DIR>`).",
        "Vérifie : `python <dossier de Silex>\\mcp_server.py --check`. Il contrôle Python, les modules, `silex.exe`, "
        "la connexion et la version du protocole, et affiche OK ou ce qui manque.",
    ])
    m.p("`mcp_server.py` est copié à côté de `silex.exe` : il le trouve tout seul, sans chemin à régler. Si Silex est "
        "ailleurs, indique-le avec la variable `SILEX_EXE`.")
    m.table(["Famille", "Outils"], [
        ["Document", "`new_document`, `open_document`, `get_active_document_info`, `save_design` (les deux premiers refusent d'effacer un travail non enregistré, sauf `discard_changes=true`)"],
        ["Inspection", "`get_model_tree`, `inspect_body` (volume, validité), `query_topology` (faces et arêtes), `list_solids`, `capture_viewport`, `verify_dimensions` (cotes finies comparées aux cotes voulues)"],
        ["Esquisse", "`create_sketch`, `sketch_add_geometry`, `sketch_add_constraints`, `sketch_import_vector` (SVG / DXF), `sketch_set_canvas` (image de référence)"],
        ["Opérations", "`extrude`, `revolve`, `fillet_or_chamfer`, `shell`, `combine`, `mirror`, `linear_pattern`, `circular_pattern`, `sweep`, `loft`, `create_box`, `create_cylinder`, `press_pull`, `create_rib`, `create_thread`, `create_emboss_text`"],
        ["Corps et faces", "`remove_body`, `defeature_faces`, `split_body`, `transform_body`, `align_bodies`, `create_construction_plane`"],
        ["Makers", "`create_fastener_hole`, `list_components`, `insert_component`, `create_enclosure`, `lay_flat_on_bed`, `check_build_volume`, `check_printability`"],
        ["Paramètres", "`get_parameters`, `set_parameter`, `delete_parameter` ; `update_feature_parameter` accepte une `expression`"],
        ["Analyse", "`measure`, `check_interference`, `set_body_material`"],
        ["Historique", "`undo`, `redo`, `begin_command_group` / `end_command_group` (plusieurs opérations de l'IA = un seul `Ctrl+Z` pour toi)"],
        ["Fichiers", "`import_step`, `export_step`, `export_stl`, `export_3mf`, `export_drawing`"],
        ["Pédagogie", "`get_pedagogical_context` (mode tuteur, § 4.28 et 4.35)"],
    ], [3.2, 13.8])
    m.p("Quand une demande échoue, Silex répond avec un **code d'erreur** (`UNKNOWN_ID`, `REBUILD_FAILED`, "
        "`DOCUMENT_DIRTY`…) et un **conseil** (`hint`) : l'assistant comprend ce qui ne va pas et corrige lui-même. "
        "Une modification qui casserait la pièce est refusée et annulée.")
    m.box("tip", "Exemple de demande : « Avec le serveur silex, dessine un boîtier de 80 × 50 × 30 mm à parois de "
                 "2 mm, ajoute 4 trous de fixation Ø3, vérifie qu'il est valide, montre-moi une capture et exporte-le en STL. »")
    m.h2("4.13 Ligne de commande")
    m.table(["Option", "Effet"], [
        ["`silex.exe fichier.silex`", "Ouvre un projet."],
        ["`--demo`", "Charge une pièce de démonstration."],
        ["`--headless`", "Démarre sans fenêtre, uniquement pour le pilotage par IA (rendu hors écran)."],
        ["`--pipe <nom>` / `--no-ipc`", "Change le nom du canal de pilotage / désactive le pilotage."],
        ["`--screenshot image.png`", "Ouvre, attend le calcul, enregistre une capture de la fenêtre puis quitte."],
        ["`--edit \"<fonction>\"`", "Ouvre directement l'éditeur d'une fonction (pour les captures et démonstrations)."],
        ["`--tool <outil>`", "Ouvre un outil au démarrage : `measure`, `interference`, `section`, `drawing`, `parameters`, `properties` "
                             "(et, pour la documentation, `essential`, `bed`, `hole`, `components`, `enclosure`, `recorder`, `ghost`)."],
        ["`--size 1366x768`", "Taille de la fenêtre (pour vérifier l'affichage sur un petit écran)."],
        ["`--tutor-mode`", "Tuteur IA socratique : l'assistant connecté peut observer, pas modéliser (§ 4.35)."],
        ["`--doctor`", "Vérifie l'installation et affiche un rapport `[OK]` / `[WARN]` / `[FAIL]` (§ 4.40)."],
        ["`--no-recovery`", "Ne propose pas de récupérer les sauvegardes automatiques au démarrage."],
        ["`--recover <fichier>`", "Restaure directement une sauvegarde automatique (`*.autosave.json`), sans question."],
    ], [5.8, 11.2])

    # ------------------------------------------------ v0.3
    m.h2("4.14 Nouveautés de la version 0.3")
    m.p("La version 0.3 ajoute quatre familles d'outils : le **contrôle mécanique** (mesure, interférences, coupe, "
        "paramètres), la **modélisation avancée** (balayage et lissage), la **mise en plan 2D** et les **matériaux** "
        "avec l'export **3MF** multi-corps.")
    m.image(IMG / "v03_overview.png", "Un boîtier en PETG noir, son couvercle en résine translucide, un tuyau balayé et un "
            "adaptateur carré-rond lissé", width_cm=15)

    m.h2("4.15 Paramètres globaux (Ctrl+P)")
    m.p("Au lieu de taper « 2 » dans dix champs différents, donne un nom à la valeur : `epaisseur_paroi = 2`. "
        "Tu tapes ensuite `epaisseur_paroi` (ou `epaisseur_paroi * 2`, `largeur - 2 * marge`…) dans n'importe quel champ "
        "numérique : cote d'esquisse, distance d'extrusion, rayon de congé, épaisseur de coque, nombre et pas d'une répétition.")
    m.steps([
        "`Ctrl+P` (ou le bouton **Parameters**) ouvre le tableau : **Name**, **Expression**, **Computed value**, **Unit**, **Comment**.",
        "**Add** crée une ligne. Une expression peut utiliser les autres paramètres, `+ - * / ^ ( )`, `sin cos tan` (en degrés), "
        "`sqrt abs min max round`, et des unités (`10 mm + 1 cm`).",
        "**OK** ou **Apply** : toutes les fonctions qui utilisent un paramètre sont recalculées en cascade.",
    ])
    m.image(IMG / "v03_parameters.png", "Le tableau des paramètres globaux", width_cm=14)
    m.box("tip", "Pour un assemblage imprimé en 3D, crée un paramètre `jeu_montage = 0.25` et utilise-le partout où deux "
                 "pièces s'emboîtent. Si l'imprimante est plus ou moins précise, une seule valeur à changer.")
    m.box("warn", "Une référence circulaire (`a = b + 1`, `b = a * 2`) ou un nom inconnu est signalé en rouge et refusé.")

    m.h2("4.16 Mesurer (I)")
    m.steps([
        "Appuie sur `I` (ou **Measure**), puis clique une ou deux faces ou arêtes (coche **Pick whole bodies** pour mesurer entre deux pièces).",
        "L'inspecteur affiche la **distance minimale** et ses composantes **ΔX ΔY ΔZ**, l'**angle** entre deux faces planes ou deux arêtes "
        "droites, le **Ø**, le **R** et le **centre** d'une arête circulaire ou d'un cylindre.",
        "Une cote temporaire jaune s'affiche dans la vue. Un troisième clic démarre une nouvelle mesure. `Échap` ferme l'outil.",
    ])
    m.image(IMG / "v03_measure.png", "Jeu de 0,25 mm mesuré entre le couvercle et le boîtier", width_cm=15)
    m.p("Quand **une seule pièce** est sélectionnée (double-clic), l'inspecteur affiche en permanence son **volume**, sa "
        "**surface**, sa **boîte englobante** et sa **masse estimée** d'après la densité de son matériau (PLA par défaut).")

    m.h2("4.17 Vérifier l'emboîtement (Maj+I)")
    m.p("L'outil d'interférences vérifie **chaque paire** de pièces sélectionnées (ou toutes) : les volumes qui se "
        "chevauchent apparaissent en **rouge** dans une vue « rayons X », avec leur volume exact. Pour les pièces qui ne se "
        "touchent pas, Silex mesure le **jeu** et applique le code couleur de l'impression FDM :")
    m.table(["Couleur", "Jeu", "Signification"], [
        ["**Rouge**", "collision", "Les pièces se chevauchent : impossible à assembler."],
        ["**Orange**", "0 à 0,15 mm", "Très serré : risque de pièces soudées ou bloquées après impression."],
        ["**Vert**", "≥ 0,15 mm", "Jeu fonctionnel valide."],
    ], [2.6, 3.4, 11.0])
    m.image(IMG / "v03_interference.png", "Le boîtier est trop haut de 1,75 mm : la lèvre du couvercle le chevauche (672 mm³ en rouge)",
            width_cm=15)

    m.h2("4.18 Vue en coupe (Alt+S)")
    m.p("`Alt+S` tranche l'affichage selon le plan **XY**, **XZ**, **YZ** ou une **face plane sélectionnée**. Le curseur "
        "**Offset** déplace le plan, **Flip** garde l'autre moitié. La matière coupée est peinte en orange plein : on voit "
        "nettement les parois, les lèvres d'emboîtement et les filetages. La coupe ne modifie **ni la pièce ni la timeline**.")
    m.image(IMG / "v03_section.png", "Coupe XZ du boîtier : les parois de 2 mm apparaissent en orange", width_cm=15)

    m.h2("4.19 Balayage (Sweep, Maj+W)")
    m.p("Le balayage fait glisser un **profil fermé** le long d'une **trajectoire** : tuyaux, conduits, poignées.")
    m.steps([
        "Dessine la trajectoire dans une esquisse (lignes et arcs bout à bout), puis le profil dans une autre esquisse, "
        "**perpendiculaire** au début de la trajectoire (astuce : le champ **Plane offset** de l'esquisse décale son plan).",
        "`Maj+W` : clique le profil ; choisis la trajectoire dans **Path** (ou **Pick edges** pour suivre des arêtes de la pièce).",
        "**Orientation** : **Corrected Frenet** (par défaut) évite que le profil vrille sur une courbe 3D.",
        "Choisis l'opération (**New body**, **Join**, **Cut**, **Intersect**) puis **OK**.",
    ])
    m.image(IMG / "v03_sweep.png", "Édition d'un balayage : coude Ø10 mm, aperçu en vert", width_cm=15)

    m.h2("4.20 Lissage (Loft, Maj+L)")
    m.p("Le lissage relie **au moins deux profils** posés sur des plans différents, par exemple un carré et un cercle "
        "pour un raccord de ventilation.")
    m.steps([
        "Dessine chaque section dans sa propre esquisse, sur des plans décalés (**Plane offset**).",
        "`Maj+L` puis clique les profils **dans l'ordre** ; un numéro s'affiche sur chacun. **Up**/**Down** les réordonnent.",
        "**Ruled** donne des surfaces droites entre les sections ; décoché, la transition est lissée. **OK**.",
    ])
    m.image(IMG / "v03_loft.png", "Adaptateur carré 40 mm vers rond Ø24 mm", width_cm=15)

    m.h2("4.21 Matériaux et export 3MF (Ctrl+3)")
    m.p("Sélectionne une pièce (double-clic) : l'inspecteur propose un **matériau** (PLA Blanc, PETG Noir, ABS Gris, Résine "
        "Translucide, PCB Vert, Aluminium, Acier), une **couleur**, une **opacité** (pour voir à travers un boîtier) et la "
        "**densité** utilisée pour la masse.")
    m.image(IMG / "v03_properties.png", "Propriétés physiques et matériau du boîtier (PETG, 17,31 g)", width_cm=15)
    m.p("`Ctrl+3` exporte toutes les pièces (ou la sélection) dans **un seul fichier .3mf** : chaque pièce reste un objet "
        "séparé avec son nom et sa couleur, l'unité millimètre est écrite dans le fichier (aucune erreur d'échelle dans "
        "Cura, PrusaSlicer ou Bambu Studio) et les maillages sont étanches.")

    m.h2("4.22 Mise en plan 2D (Ctrl+D)")
    m.p("`Ctrl+D` ouvre la feuille de plan : les **3 vues** normalisées en projection européenne (vue de face, vue de dessus "
        "en dessous, vue de droite à gauche), les arêtes **cachées en tirets**, les **axes** en trait mixte rouge, un cadre "
        "et un **cartouche** (titre, échelle, unité, date, volume, masse, matériau).")
    m.steps([
        "Choisis **A4** ou **A3** et l'**échelle** (Auto choisit 2:1, 1:1, 1:2… pour que tout tienne).",
        "Outils de cotation : **Linear** (clique deux sommets, puis la position de la cote ; `Maj` pour une cote alignée), "
        "**Diameter** et **Radius** (clique un cercle puis la position du texte). Les points s'accrochent aux sommets et centres.",
        "Clique une cote puis `Suppr` pour l'effacer. Remplis le cartouche à droite.",
        "**Export PDF** ou **Export SVG**. Quand tu modifies la pièce 3D, le plan et ses cotes se mettent à jour.",
    ])
    m.image(IMG / "v03_drawing.png", "Mise en plan de l'équerre de référence à l'échelle 1:2", width_cm=15)

    m.h2("4.23 Esquisse Pro : contraintes, formes et outils d'édition 2D")
    m.p("L'esquisseur dispose des outils des modeleurs paramétriques de référence : des formes « intelligentes » déjà "
        "contraintes, des outils pour découper, prolonger, arrondir, décaler, symétriser et répéter, et la saisie des "
        "cotes **pendant** le tracé.")
    m.image(IMG / "sketchpro_editor.png", "Rectangle centré avec congés R6, oblong, logement hexagonal Ø13 sur plats, "
            "perçages symétriques (badge Sym) ; grille magnétique dans la barre d'état", width_cm=16.5)
    m.h3("Nouvelles contraintes")
    m.table(["Contrainte", "Touche", "Sélection et effet"], [
        ["Milieu (Mid)", "`M`", "Un point + une ligne (ou un arc) : le point se place au milieu exact."],
        ["Symétrie (Sym)", "`S`", "Deux points (ou deux lignes, cercles, arcs), puis **en dernier** la ligne d'axe (trait de construction ou axe X / Y). Deux cercles symétriques ont aussi le même diamètre."],
        ["Concentrique (◎)", "`O`", "Deux cercles ou arcs (y compris une arête projetée) : même centre."],
        ["Colinéaire (Col)", "bouton", "Deux lignes sur la même droite."],
    ], [3.6, 1.8, 11.6])
    m.h3("Formes intelligentes")
    m.table(["Outil", "Accès", "Comment l'utiliser"], [
        ["Rectangle par centre", "`Maj+R` ou ▾ sous Rectangle", "Clic au centre, clic sur un coin. Les diagonales de construction et la contrainte Milieu le gardent centré : 2 cotes suffisent."],
        ["Polygone", "▾ sous Rectangle", "Choisis le nombre de côtés (3 à 12, 6 par défaut) et le mode dans le panneau, clic au centre, clic sur un sommet (inscrit) ou au milieu d'un côté (circonscrit : idéal pour un écrou, cote sur plats)."],
        ["Oblong (Slot)", "`U`", "Centre à centre : clic centre 1, clic centre 2, clic pour la largeur. Par centre global : clic au centre, clic au bout de l'axe, clic pour la largeur. Côtés tangents et demi-cercles égaux déjà appliqués."],
    ], [3.4, 3.4, 10.2])
    m.h3("Outils d'édition 2D")
    m.table(["Outil", "Accès", "Comment l'utiliser"], [
        ["Ajuster (Trim)", "`T`", "Survole une courbe : la portion qui sera supprimée apparaît en rouge pointillé. Clique pour la couper aux intersections ; les contraintes valides restent."],
        ["Prolonger (Extend)", "▾ sous Trim", "Clique près de l'extrémité d'une ligne ou d'un arc : il est prolongé jusqu'à la prochaine courbe (aperçu vert)."],
        ["Congé d'esquisse", "`Maj+F`", "Clique le coin commun à deux lignes (ou les deux lignes) : un arc tangent est inséré et sa cote de rayon s'ouvre aussitôt. Les cotes de longueur deviennent des cotes « au coin virtuel »."],
        ["Décaler (Offset)", "`Maj+O`", "Sélectionne un contour, tape la distance (négative = de l'autre côté), `Entrée`. Une seule cote pilote toute la copie."],
        ["Miroir 2D", "bouton Mirror", "Sélectionne les éléments puis, en dernier, la ligne de symétrie : les copies sont liées par des contraintes Sym."],
        ["Répétition 2D", "bouton Pattern", "Sélectionne les éléments (et un point centre pour une répétition circulaire), tape le nombre, `Tab`, le pas ou l'angle, `Entrée`. Le pas est une cote modifiable."],
    ], [3.4, 2.8, 10.8])
    m.h3("Assistance en temps réel")
    m.bullets([
        "**Cotes à la volée** : pendant le tracé d'une ligne, d'un rectangle, d'un cercle, d'un polygone ou d'un oblong, "
        "tape directement une valeur. Des champs flottants suivent le curseur (`Tab` pour passer de *Largeur* à *Hauteur*, "
        "`Entrée` pour valider). La cote correspondante est créée sans repasser par `D`. Les noms de paramètres globaux "
        "sont acceptés (ex. `entraxe_x / 2`).",
        "**Badges de contraintes** : `H`, `V`, `T`, `⊥`, `Sym`, `Mid`… s'affichent près des éléments. Survole un badge "
        "pour surligner les éléments qu'il relie ; clique-le puis `Suppr` pour supprimer cette seule contrainte.",
        "**Grille magnétique** : bouton **Snap** et pas (0,5 / 1 / 5 mm) en bas à droite de la barre d'état. "
        "Maintiens `Alt` pour la désactiver le temps d'un clic.",
        "**Show under-constrained** : fait pulser en bleu les éléments qui ont encore des degrés de liberté.",
    ])
    m.box("tip", "Pour un logement d'écrou M6 : `Polygone`, mode *Circumscribed*, clic au centre, tape `6`, `Tab`, `5.05`, "
                 "`Entrée` : hexagone de 10,1 mm sur plats, déjà coté.")

    m.h2("4.24 Navigation et modélisation directe")
    m.h3("Le ViewCube")
    m.p("Le cube d'orientation en haut à droite de la vue suit la caméra en temps réel. Ses libellés suivent la langue "
        "du système (`TOP` / `DESSUS`, `FRONT` / `FACE`…).")
    m.bullets([
        "Clique une **face** pour une vue standard, une **arête** (bande au bord d'une face) pour une vue à 45°, un "
        "**coin** pour une vue isométrique. La zone survolée s'allume en bleu, la caméra pivote en douceur (~200 ms).",
        "Le bouton **Maison** (en haut à gauche du cube) revient à la vue isométrique et cadre toute la pièce.",
        "Quand la vue est alignée sur une face, deux **flèches courbes** font pivoter la vue de 90° dans un sens ou dans l'autre.",
    ])
    m.h3("Pousser / Tirer (Press/Pull, `Q`)")
    m.p("Un seul outil pour ajouter ou enlever de la matière, sans repasser par une esquisse :")
    m.bullets([
        "sur un **profil d'esquisse fermé**, `Q` lance l'**extrusion** habituelle (New body, Join, Cut) ;",
        "sur une **face plane** d'une pièce, `Q` pousse ou tire directement la face : tire la **flèche orange** ou tape "
        "une distance. L'aperçu est **bleu** si tu ajoutes de la matière, **rouge** si tu en retires.",
    ])
    m.image(IMG / "ergo_presspull.png", "Press/Pull de +8 mm sur la face du dessus : flèche orange, aperçu bleu, "
            "ViewCube en haut à droite", width_cm=16.5)
    m.p("L'opération devient une étape de la timeline (icône bloc + flèche) : double-clique pour changer la distance, "
        "les étapes suivantes se recalculent.")
    m.h3("Primitives rapides : Box (`Maj+B`) et Cylinder (`Maj+Y`)")
    m.p("Menu déroulant **Box ▾** de la barre *Modèle*. Une boîte ou un cylindre est créé directement, sans esquisse :")
    m.table(["Paramètre", "Effet"], [
        ["Plan de base", "XY, XZ, YZ, ou une face plane cliquée dans la vue (le repère part alors du centre de la face)."],
        ["Centre X / Y", "Position du centre de la base dans ce plan."],
        ["Longueur, Largeur / Diamètre", "Taille de la base (Box : X et Y ; Cylinder : Ø)."],
        ["Hauteur (Z)", "Le long de la normale du plan (négative = de l'autre côté). Aussi pilotable avec la flèche."],
        ["Opération", "New body, Join (bossage) ou Cut (perçage, poche)."],
    ], [4.5, 12.5])
    m.image(IMG / "ergo_box.png", "Éditeur de la primitive Box : tous les champs acceptent les paramètres globaux", width_cm=16.5)
    m.h3("La barre d'état")
    m.table(["Indicateur", "Signification"], [
        ["● MCP", "**Gris** : canal IA prêt, aucun agent connecté. **Vert** : un agent (Claude Code, Gemini CLI…) pilote "
                  "Silex. **Orange** : canal déjà occupé par une autre instance (IPC désactivé)."],
        ["Triangles", "Nombre de triangles du maillage affiché : un bon indicateur de la finesse avant un export STL ou 3MF."],
        ["compute … ms", "Durée du dernier recalcul du modèle."],
        ["Snap", "Clic : active / coupe la grille magnétique de l'esquisse ; flèche : Off, 0,5, 1, 2 ou 5 mm."],
    ], [3.2, 13.8])

    m.h2("4.25 v0.4 — Gérer les corps et les faces directement")
    m.table(["Action", "Comment", "Ce qui se passe"], [
        ["Supprimer un corps", "Double-clic sur la pièce, `Suppr`", "Une étape **RemoveBody** est ajoutée à la timeline : les fonctions qui ont créé la pièce restent intactes, `Ctrl+Z` la fait revenir."],
        ["Masquer / montrer", "`V` (rien de sélectionné : tout réapparaît), œil dans l'Inspecteur", "Affichage seulement ; une pièce masquée ne se sélectionne plus. Clic droit > **Isolate** / **Unisolate**."],
        ["Supprimer une face", "Clic sur la face (congé, chanfrein, poche, trou), `Suppr`", "Étape **DeleteFace** : la face disparaît et les faces voisines se prolongent pour refermer la pièce. Fonctionne aussi sur un STEP importé sans historique."],
        ["Scinder un corps", "Barre *Modèle* > **Split Body**", "La pièce est coupée par un plan (XY / XZ / YZ décalé, face plane ou plan de construction) en `<Nom>_1` (sous le plan) et `<Nom>_2`. Idéal pour faire fond + couvercle d'un boîtier fermé."],
        ["Déplacer / copier", "Clic droit > **Move / Copy**", "Gizmo : 3 flèches (translation) et 3 arcs (rotation), `Ctrl` pour arrondir au mm / à 15°, champs ΔX…Rz (paramètres acceptés), case **Create copy**."],
    ], [3.2, 4.6, 9.2])
    m.image(IMG / "v04_move.png", "Move / Copy : gizmo de translation et de rotation, fantôme du couvercle déplacé", width_cm=16.5)

    m.h2("4.26 v0.4 — Plans de construction, nervures, alignement")
    m.bullets([
        "**Plane ▾** : plan *décalé* (d'un plan ou d'une face, distance pilotable), *incliné* (d'un angle autour de X / Y / Z ou "
        "d'une arête droite) ou *milieu* (entre deux faces parallèles). Il apparaît en orange dans la vue et dans le dossier "
        "**Construction** de l'Inspecteur (bouton *Sketch* pour esquisser dessus). Esquisses, primitives et Split Body l'acceptent.",
        "**Rib** (nervure) : dessine une simple ligne ouverte dans une esquisse qui coupe la pièce (par exemple un plan au milieu "
        "d'une équerre), d'une paroi à l'autre ; la nervure d'épaisseur choisie remplit l'espace jusqu'aux parois.",
        "**Align** : clique une face du corps à déplacer puis une face du corps fixe. Mode *face à face* avec un jeu "
        "(`jeu_montage` = 0,25 mm par exemple, centres alignés) ou *coaxial* (deux trous de vis). **Flip** inverse le sens. "
        "L'alignement est recalculé si les pièces changent.",
    ])

    m.h2("4.27 v0.4 — Assistant d'impression 3D et filetages réels")
    m.p("**Printability** (barre *Modèle*) colore les faces en porte-à-faux selon leur angle par rapport à la verticale : "
        "orange de 45 à 60°, rouge au-delà (supports nécessaires), et en magenta les parois plus fines que le minimum "
        "(0,8 mm = 2 passes d'une buse 0,4). Change la direction d'impression pour trouver l'orientation sans supports : "
        "un « T » imprimé à l'envers n'en demande plus aucun.")
    m.image(IMG / "v04_printability.png", "Boîte à parois de 0,5 mm signalée en magenta, bloc plein correct", width_cm=16.5)
    m.p("**Thread** : clique la face cylindrique d'une tige (vis, modélisée au diamètre nominal : M6 → Ø6) ou d'un trou "
        "(écrou, modélisé au diamètre de perçage : M6 → Ø5). Tailles M3 à M12, pas normal ou fin, **jeu FDM** (0,15 mm par "
        "défaut) pour qu'une vis et un écrou imprimés se vissent sans forcer. Les filets de deux pièces coaxiales "
        "s'engrènent toujours. Mode *cosmétique* : simple annotation (anneaux pointillés + « M6x1 »), sans géométrie.")
    m.box("tip", "Après avoir fileté une vis et son écrou, lance `Maj+I` : la vérification d'interférences doit être "
                 "verte, avec un jeu de 0,15 mm.")

    m.h2("4.28 v0.4 — Apprendre avec Silex : mode guidé et diagnostic")
    m.bullets([
        "**Learn** (`F1`) ouvre le panneau *Apprendre / Mode guidé* : trois parcours (l'équerre — débutant, le boîtier à "
        "couvercle — intermédiaire, la bride et sa mise en plan — avancé). Chaque étape se coche **toute seule** dès que "
        "ton modèle remplit l'objectif, quelle que soit ta façon de dessiner. Le bouton **Hint** fait clignoter l'outil "
        "attendu sans faire le travail à ta place.",
        "Dans une esquisse, **Expliquer** dit en clair ce qui manque : « Ce rectangle a ses bonnes dimensions (40 × 85 mm), "
        "mais il peut encore glisser en X et Y. Attache son centre à l'origine avec la contrainte Milieu (M)… », et fait "
        "pulser les éléments encore bleus.",
        "Une étape en rouge est expliquée dans l'Inspecteur (💡) : rayon de congé trop grand pour la hauteur disponible, "
        "contour non fermé, profil qui traverse l'axe d'une révolution, coque trop épaisse…",
        "**Guided tour** : sur un fichier annoté (`annotation` sur chaque étape), le marqueur avance étape par étape et "
        "affiche l'intention de conception de chaque fonction.",
        "Langue des explications : FR / EN en haut du panneau.",
    ])
    m.image(IMG / "v04_learn.png", "Panneau Apprendre : parcours de l'équerre, critères validés automatiquement", width_cm=16.5)
    m.image(IMG / "v04_tour.png", "Visite guidée d'une équerre annotée : étape 1 / 14 et son intention de conception", width_cm=16.5)
    m.p("**Mode tuteur IA** : demande à ton assistant « Active le mode tuteur : guide-moi pour modéliser un support mural "
        "sans jamais dessiner à ma place ». L'outil MCP `get_pedagogical_context` lui donne les degrés de liberté "
        "expliqués, les erreurs récentes et l'étape de la leçon en cours.")

    m.h2("4.29 Finitions : barre d'outils, sélection et raccourcis")
    m.bullets([
        "La barre *Modèle* tient sur un écran de 1366 px : les outils secondaires sont rangés dans **Create ▾** (Box, "
        "Cylinder, Rib, Hole / Insert, Text, Insert Component, Enclosure Wizard),**Modify ▾** (Fillet, Chamfer, Shell, Split Body, Delete Face, Move/Copy, Align), "
        "**Pattern ▾** (Mirror, Linear, Circular), **Construct ▾** (Offset / Angled Plane, Midplane) et **Inspect ▾** "
        "(Measure, Interference, Section, Printability). Leurs raccourcis clavier marchent toujours directement.",
        "Couleurs : une sélection en esquisse est **cyan vif** et plus épaisse ; le survol (2D et 3D) est **jaune clair**. "
        "Blanc = contraint, bleu = encore libre, orange pointillé = construction, rouge = sur-contraint.",
        "**Sélection par cadre** en glissant dans le vide : vers la **droite** (cadre bleu plein) seuls les éléments "
        "entièrement dedans sont pris ; vers la **gauche** (cadre vert pointillé) tout ce que le cadre touche. "
        "En 3D, le cadre sélectionne des pièces.",
        "En esquisse, **N** (ou le bouton œil à côté d'*Expliquer*) remet la vue bien à plat face au plan. "
        "**Ctrl+A** sélectionne toute l'esquisse. Le **clic droit** sur une sélection propose directement les contraintes "
        "compatibles (2 points + 1 ligne → Symétrie, Milieu…). Le bas du panneau résume tous les raccourcis.",
    ])
    m.image(IMG / "v04f_toolbar_1366.png", "Barre d'outils regroupée : tout tient en 1366 × 768", width_cm=16.5)
    m.image(IMG / "v04f_sketch_selection.png", "Sélection en esquisse : cyan vif, trait de 3 px", width_cm=16.5)

    m.h2("4.30 Les 9 parcours guidés et leurs exemples annotés")
    m.p("Le menu du panneau **Learn** (`F1`) range les parcours par niveau. Chaque parcours a un bouton "
        "**Ouvrir l'exemple annoté** : la pièce finie s'ouvre et la visite guidée explique chaque étape (FR / EN).")
    m.table(["Parcours", "Niveau", "Ce qu'on apprend"], [
        ["1 — Équerre de fixation", "Débutant", "esquisse contrainte, extrusion, perçage, congé, modification paramétrique"],
        ["4 — Dé à jouer paramétrique", "Débutant", "rectangle par le centre, esquisses sur les 6 faces, répétition 2D, congés sur 12 arêtes, paramètre « cote »"],
        ["2 — Boîtier à couvercle", "Intermédiaire", "paramètre de jeu, coque, Split Body, Align, interférences"],
        ["5 — Cadre de pince de serrage", "Intermédiaire", "profil en C avec congés 2D, nervures, bossage, taraudage M8 avec jeu FDM"],
        ["6 — Poulie à gorge", "Intermédiaire", "demi-profil et révolution, alésage, rainure de clavette (oblong), répétition circulaire, mise en plan"],
        ["7 — Support de roulement", "Intermédiaire", "demi-pièce + symétrie, trou oblong de réglage, logement concentrique (O), trou de graissage"],
        ["3 — Bride et mise en plan", "Avancé", "révolution, répétition, taraudage, plan PDF"],
        ["8 — Raccord carré-rond", "Avancé", "plan décalé, lissage carré → cercle, coque ouverte aux deux bouts, Printability"],
        ["9 — Coude à brides", "Avancé", "balayage le long d'un arc, brides percées, paramètres diametre_tube / epaisseur_paroi, export 3MF et PDF"],
    ], [4.6, 2.6, 9.8])
    m.image(IMG / "v04f_learn_list.png", "Le menu Learn : 9 parcours classés Débutant / Intermédiaire / Avancé", width_cm=16.5)
    m.image(IMG / "v04f_cas_ecole.png", "Quatre des cas d'école : pince, poulie, support de roulement, raccord carré-rond", width_cm=16.5)

    # ---------------------------------------------------------------- v0.5 Makers & Pedagogy
    m.h2("4.31 Perçages normalisés et inserts (Hole / Insert, Maj+H)")
    m.p("Plus besoin de chercher les cotes d'un lamage ou d'un insert laiton : clique une face plane, le trou se pose "
        "où tu cliques. Il s'accroche au centre de la face, aux points d'esquisse, aux axes des trous existants "
        "(par exemple les trous d'une carte insérée) et à la grille. **Ctrl+clic** ajoute un autre trou dans la même étape.")
    m.table(["Type", "Ce que Silex perce (M3)", "Pour"], [
        ["Passage (clearance)", "Ø3,2 serré ou Ø3,4 normal, traversant ou borgne", "une vis qui traverse librement"],
        ["Lamage (counterbore)", "Ø3,4 + logement Ø6,1 × 3,2", "vis à tête cylindrique CHC (DIN 912)"],
        ["Fraisage (countersink)", "Ø3,4 + cône 90° Ø6,4", "vis à tête fraisée FHC (DIN 7991), tête affleurante"],
        ["Insert thermofusible", "Ø4,0 × 5,7 avec chanfrein d'entrée 0,4", "insert laiton posé au fer (type Ruthex / CNC Kitchen)"],
        ["Écrou prisonnier", "hexagone 5,5 + 2 × 0,15 de jeu, puis passage Ø3,4", "écrou M3 glissé dans la pièce"],
    ], [3.8, 6.2, 7.0])
    m.p("Tailles M2 à M6. C'est une étape paramétrique `FastenerHole` de la timeline (double-clic pour la modifier), "
        "et l'outil MCP `create_fastener_hole` fait la même chose.")
    m.image(IMG / "v05_hole.png", "Outil Hole / Insert sur le couvercle : type, taille, ajustement, profondeur", width_cm=16.5)
    m.h3("Bibliothèque de composants (Create ▾ > Insert Component)")
    m.p("Raspberry Pi 4B et 5, Raspberry Pi Pico, ESP32 DevKit V1, Arduino Uno R3, écran OLED 0,96\", bouton 6 × 6, "
        "embase USB-C et ventilateur 40 × 40 × 10. Ce sont des modèles simplifiés aux cotes officielles : contour, "
        "trous de fixation, encombrement des connecteurs.")
    m.bullets([
        "Le composant arrive comme **corps de référence verrouillé** 🔒, avec son matériau (PCB vert…). On le place "
        "avec *Move/Copy* ou *Align*, on projette ses arêtes dans une esquisse et on vérifie les interférences.",
        "Il ne peut pas être modifié : une étape qui tenterait de le percer ou de le fusionner passe en rouge, avec "
        "un message clair. Un *Join* sans cible choisit automatiquement ta pièce, pas la carte.",
        "Il n'est ni exporté (STL, 3MF, STEP « tout ») ni contrôlé par *Printability* : on n'imprime pas la carte.",
    ])
    m.image(IMG / "v05_composants.png", "La bibliothèque : description, vis, entretoises et trous de fixation de chaque carte", width_cm=12)

    m.h2("4.32 Impression 3D sans échec")
    m.bullets([
        "**Bed** (barre d'état) : affiche le plateau et le volume d'impression (Prusa MK4 / Ender 3 220 × 220 × 250, "
        "Bambu Lab P1 / X1 256³, Prusa Mini 180³ ou taille personnalisée). Une pièce qui dépasse passe le volume en "
        "**rouge**, avec son nom en bas de la vue.",
        "**Poser à plat** (Modify ▾ > Lay Flat on Bed, ou clic droit sur une face) : clique la face qui doit toucher le "
        "plateau, la pièce se couche dessus, centrée, prête pour le trancheur. C'est une étape recalculée avec le modèle.",
        "**Split Body avec pions** : coche *Add alignment pins / pegs*. Silex pose 2 pions (ou plus) sur la moitié du "
        "bas, aussi écartés que possible dans la section, et perce les logements dans l'autre moitié avec 0,20 mm de "
        "jeu radial. Les deux impressions se recollent sans décalage.",
        "**Assistant boîtier** (Create ▾ > Enclosure Wizard) : dimensions intérieures, paroi, rayon des coins et "
        "fermeture (lèvre emboîtable, 4 colonnettes M3 avec inserts, ou couvercle coulissant). Ce n'est pas un bloc "
        "opaque : il écrit une **timeline commentée** (Box → Fillet → Shell → Split → fermeture) et des paramètres "
        "(`int_longueur`, `paroi`…). On change une cote dans `Ctrl+P` et tout le boîtier suit.",
    ])
    m.image(IMG / "v05_assistant_boitier.png", "L'assistant boîtier : 3 clics", width_cm=7)
    m.image(IMG / "v05_boitier_pi.png", "Boîtier généré (colonnettes M3, inserts) avec un Raspberry Pi 4 verrouillé sur ses entretoises", width_cm=16.5)
    m.image(IMG / "v05_plateau.png", "Plateau Prusa Mini : la pièce de 200 mm dépasse, le volume passe en rouge", width_cm=16.5)

    m.h2("4.33 Défis de dépannage et fantôme avant / après")
    m.p("Dans **Learn** (`F1`), la catégorie **Défis de dépannage (5 min)** ouvre une pièce cassée à réparer. Le défi se "
        "valide tout seul quand le but est atteint **et** que le *test de robustesse* passe : Silex change lui-même une "
        "cote sur une copie de la pièce, la recalcule et revérifie.")
    m.table(["Défi", "Le problème", "Ce que vérifie Silex"], [
        ["1 — L'esquisse élastique", "les trous se décentrent quand la largeur passe de 60 à 80 mm",
         "esquisse 0 DOF ; à 80 mm, les trous restent symétriques"],
        ["2 — Le couvercle bloqué", "collision rouge entre bac et couvercle (`jeu_montage` négatif)",
         "jeu 0,1 à 0,5 mm, aucune collision, même boîte rallongée à 80 mm"],
        ["3 — Le congé impossible", "un congé plus grand que l'épaisseur (étape rouge)",
         "le congé existe et passe, même avec une plaque de 6 mm"],
        ["4 — Le contour ouvert", "l'extrusion n'a plus de profil fermé",
         "extrusion verte, volume complet, contour toujours fermé à 55 mm de haut"],
        ["5 — La pièce STEP à modifier", "bossage inutile et trou mal placé, sans historique",
         "bossage supprimé (Delete Face), trou déplacé à X = −15 mm"],
    ], [4.0, 6.3, 6.7])
    m.image(IMG / "v05_defi4.png", "Défi 4 : l'extrusion est rouge, les critères attendent la réparation", width_cm=16.5)
    m.p("**Fantôme avant / après** : quand tu modifies une cote, une étape de la timeline ou un paramètre (`Ctrl+P`), "
        "la silhouette filaire **gris pâle** de l'état précédent reste affichée pendant toute l'édition, puis 3 secondes. "
        "On voit tout de suite le sens et l'ampleur de la modification.")
    m.image(IMG / "v05_fantome.png", "Le paramètre « cote » passe de 20 à 26 : le fantôme montre le dé d'avant", width_cm=16.5)

    m.h2("4.34 Interface Essentiel / Complet et cartes d'intention")
    m.bullets([
        "Le bouton tout à droite de la barre *Modèle* bascule entre **Essentiel** (débutants, ateliers) et **Complet**. "
        "En Essentiel, la barre ne garde que les 7 commandes fondamentales : Sketch, Extrude, Press/Pull, "
        "Fillet / Chamfer, Hole / Insert, Measure, Export STL/3MF (plus Fit et Learn). La barre d'esquisse garde les "
        "outils de base, et les options avancées des panneaux (Intersect…) sont masquées.",
        "La bascule est instantanée et ne touche jamais au document ; les raccourcis clavier marchent dans les deux profils.",
        "**Cartes d'intention** : laisse la souris 1,2 s sur un outil (ou une entrée de menu). Une carte montre le nom "
        "FR / EN, le raccourci, une miniature **avant → après** et une phrase en langage maker.",
    ])
    m.image(IMG / "v05_essentiel.png", "Profil Essentiel : 7 commandes", width_cm=16.5)
    m.image(IMG / "v05_carte_intention.png", "Carte d'intention de la coque : « Creuse un volume plein en gardant une épaisseur de paroi constante »", width_cm=16.5)

    m.h2("4.35 Formateurs : enregistreur de leçons et tuteur IA socratique")
    m.bullets([
        "**Créer une leçon depuis ce projet** (panneau Learn) : Silex lit la timeline de ta pièce et propose une étape "
        "par fonction, avec des critères automatiques. Ce sont le type d'étape, l'esquisse entièrement contrainte, les "
        "cotes clés (distance, rayon, épaisseur, taille M3…), puis le nombre de corps et le volume final (± 2 %).",
        "Tu écris la consigne et l'indice de chaque étape (FR / EN), puis **Exporter le pack** (`.json` + `.silex`) pour "
        "le distribuer, ou **Ajouter à mes leçons** : la leçon apparaît dans le menu Learn, avec la pièce de référence "
        "pour la visite guidée.",
        "**Tuteur IA (mode socratique)** (bouton du panneau Learn, ou `silex --tutor-mode`) : l'assistant connecté par "
        "MCP ne peut plus qu'**observer** (arbre, degrés de liberté, captures, mesures). Silex refuse ses commandes de "
        "modélisation : il guide l'élève par des questions et des indices, sans faire à sa place. La barre d'état "
        "affiche « · tutor ». Le pont MCP lancé avec `--tutor-mode` n'expose que les outils de lecture.",
    ])
    m.image(IMG / "v05_enregistreur.png", "L'enregistreur : une étape par fonction, consignes et indices à compléter", width_cm=16.5)
    m.image(IMG / "v05_learn_defis.png", "Le menu Learn : 9 parcours et 5 défis de dépannage", width_cm=16.5)

    # ---------------------------------------------------------------- v0.7 Trio Makers + reliability
    m.h2("4.36 v0.7 — Texte en relief ou gravé, qui suit tes paramètres (Maj+T)")
    m.p("Écris un nom, une référence ou une cote **sur** ta pièce : en relief (la matière dépasse) ou gravé (creusé "
        "dedans). Le texte peut afficher la valeur de tes paramètres globaux et se met à jour avec eux.")
    m.image(IMG / "v07_overview.png", "Texte en relief « Silex v2 - 80 mm » (il suit les paramètres), texte gravé "
            "« MAKERS » et une étoile importée d'un SVG", width_cm=15)
    m.steps([
        "**Create ▾ > Text (Emboss / Engrave)**, `Maj+T` (en dehors d'une esquisse), ou clic droit sur une face.",
        "Clique une **face plane** : le texte se centre dessus. Reclique ailleurs sur la face pour le déplacer "
        "(il s'accroche au centre). Les plans XY / XZ / YZ et les plans de construction marchent aussi.",
        "Tape le texte. Plusieurs lignes sont possibles. Le menu **{x} Parameter** insère un paramètre global entre accolades.",
        "Règle la **police**, la **hauteur** (celle des majuscules), la **profondeur**, la **rotation**, l'**alignement** "
        "(gauche, centre, droite) et la **position** X / Y. Tous ces champs acceptent les paramètres (`hauteur_texte`, `paroi / 2`…).",
        "**OK** : le texte devient une étape `Text` de la timeline (double-clic pour le modifier).",
    ])
    m.table(["Tu écris…", "Silex affiche (largeur = 80, version = 2)"], [
        ["`W={largeur}`", "W=80"],
        ["`Boîtier v{version} - {largeur} mm`", "Boîtier v2 - 80 mm"],
        ["`{largeur / 7:2}`", "11.43 (`:2` = deux décimales)"],
        ["`{{texte}}`", "{texte} (accolades doublées = accolades affichées)"],
    ], [7.0, 10.0])
    m.p("Sous le texte, la ligne **Shown as** montre le résultat. Change `largeur` dans `Ctrl+P` : la pièce s'allonge "
        "**et** le texte devient « W=100 ».")
    m.bullets([
        "**Profondeur positive** = relief (ajoute de la matière) ; **négative** = gravure (enlève de la matière).",
        "Police : **Liberation Sans** est fournie avec Silex (le fichier `.silex` s'ouvre pareil sur un autre PC). "
        "Les polices installées sur Windows sont aussi proposées.",
        "Les trous des lettres (o, e, A, 8…) sont respectés : un « o » gravé laisse bien son îlot central.",
    ])
    m.image(IMG / "v07_text_editor.png", "L'éditeur de texte : le paramètre {largeur} devient « 80 », trait de 0,77 mm "
            "imprimable avec une buse de 0,4 mm", width_cm=16.5)
    m.box("warn", "Si les traits des lettres sont **plus fins que la buse** (0,4 mm), le panneau l'écrit en orange et "
                  "l'étape passe en avertissement : l'imprimante risque de ne pas les tracer. Agrandis le texte "
                  "(au moins 3 mm de haut environ en Liberation Sans) ou choisis une police plus grasse.")

    m.h2("4.37 v0.7 — Importer un dessin SVG ou DXF dans une esquisse (Ctrl+Maj+I)")
    m.p("Récupère un logo dessiné dans Inkscape, un contour exporté d'un logiciel de découpe laser ou un plan DXF, "
        "et transforme-le en esquisse Silex que tu peux extruder.")
    m.steps([
        "Ouvre une esquisse, puis **Import SVG/DXF** dans la barre d'esquisse (ou `Ctrl+Maj+I`) et choisis le fichier.",
        "Règle la **taille** : un facteur d'échelle, ou directement la **largeur voulue** en mm. Puis le **centre** X / Y "
        "et la **rotation**.",
        "Laisse **Group as rigid block** coché (conseillé) : le dessin se déplace d'un seul bloc sans se déformer. "
        "Coche **As construction geometry** si tu veux seulement un guide pour redessiner.",
        "**OK** : la barre d'état indique le nombre de lignes, arcs et cercles importés, et combien d'extrémités ont été "
        "raccordées. Les contours fermés sont prêts à extruder.",
    ])
    m.bullets([
        "SVG : chemins (courbes de Bézier comprises), rectangles, cercles, ellipses, lignes, polylignes, polygones, "
        "transformations et unités (mm, cm, in, px).",
        "DXF (texte) : lignes, cercles, arcs, polylignes (avec arrondis), splines, ellipses ; l'unité du fichier est lue.",
        "Les courbes sont approchées à **0,02 mm** près ; deux extrémités à moins de **0,01 mm** sont collées entre elles.",
        "Un **bloc rigide** garde 2 degrés de liberté : place-le en cotant son point central (`D`) par rapport à "
        "l'origine. Supprimer ce point central libère le bloc : chaque ligne redevient modifiable.",
    ])
    m.box("tip", "Un SVG en pixels n'a pas de vraie taille. Utilise plutôt l'option **Target width** : tape la largeur "
                 "voulue en millimètres et tout le dessin est mis à l'échelle d'un coup.")

    m.h2("4.38 v0.7 — Redessiner d'après une photo : l'image de référence")
    m.p("Pour reproduire une pièce cassée, pose-la sur une feuille quadrillée à côté d'une règle, prends une photo bien "
        "de face, et glisse la photo **sous** ton esquisse pour la décalquer.")
    m.steps([
        "Dans l'esquisse, bouton **Image** de la barre (ou groupe *Import / reference image* du panneau) : choisis une "
        "photo PNG, JPG, BMP ou WebP. Elle s'affiche en transparence sous le dessin.",
        "**Calibre** l'image : clic droit > **Calibrate image (2 points)** (ou bouton **Calibrate**). Clique deux points "
        "dont tu connais la distance (deux graduations de la règle, l'entraxe de deux trous), puis tape cette distance. "
        "L'image passe à l'échelle 1:1 ; le premier point cliqué ne bouge pas.",
        "Règle l'**opacité** avec le curseur, retourne l'image avec **Flip H** / **Flip V** si la photo est en miroir.",
        "Redessine les contours par-dessus avec les outils habituels, puis cote-les.",
    ])
    m.image(IMG / "v07_canvas_sketch.png", "Photo d'une équerre sur papier quadrillé, calibrée sur la règle jaune, "
            "sous une esquisse qui contient aussi l'étoile SVG importée en bloc rigide (2 DOF)", width_cm=16.5)
    m.bullets([
        "L'image est **enregistrée dans le fichier .silex** : tu peux l'envoyer à quelqu'un, elle sera là. Au-delà de "
        "12 Mo, Silex garde seulement un lien vers le fichier image, pour ne pas alourdir le projet.",
        "L'image sert seulement de guide : elle ne change ni la pièce, ni son volume, ni les exports. Elle n'est visible "
        "que pendant l'édition de l'esquisse. **Remove image** la retire.",
    ])
    m.box("tip", "Pour une bonne mesure, photographie de loin avec le zoom (moins de déformation) et bien d'aplomb. "
                 "Calibre sur la plus grande distance possible : l'erreur de clic compte moins.")

    m.h2("4.39 Sauvegarde automatique et récupération après un plantage")
    m.bullets([
        "Toutes les **2 minutes**, si ton document a des modifications non enregistrées, Silex en fait une copie de "
        "secours dans `%LOCALAPPDATA%\\Silex\\recovery`. Ton fichier `.silex` n'est jamais touché.",
        "Quand tu enregistres ou que tu fermes Silex normalement, la copie de secours est effacée.",
        "Si Silex s'arrête brutalement (plantage, coupure de courant), il te propose au démarrage suivant de "
        "**Restaurer**, **Ignorer** (la garder pour plus tard) ou **Supprimer** chaque copie trouvée. Après une "
        "restauration, pense à enregistrer (`Ctrl+S`).",
        "Menu **?** > **Autosave** : intervalle de 1, 2, 5, 10 ou 15 minutes, ou désactivé ; **Open the recovery folder** "
        "ouvre le dossier des copies.",
    ])
    m.box("eli", "C'est comme le brouillon enregistré tout seul dans un traitement de texte : tu peux toujours perdre "
                 "les dernières minutes, mais jamais une après-midi de travail.")

    m.h2("4.40 Vérifier l'installation : silex --doctor")
    m.p("Si quelque chose ne marche pas (leçons absentes, IA qui ne se connecte pas…), lance le diagnostic dans une "
        "fenêtre PowerShell ouverte dans le dossier de Silex :")
    m.p("`.\\silex.exe --doctor | Out-Host`")
    m.p("Silex affiche une ligne par vérification, marquée **[OK]**, **[WARN]** (ça marche, mais une option manque) "
        "ou **[FAIL]** (à réparer) :")
    m.table(["Vérification", "Ce qui est contrôlé"], [
        ["Ressources", "les 9 leçons, les exemples, les 9 composants et leurs modèles STEP, les défis, la police du texte"],
        ["Moteur 3D", "OpenCascade calcule un solide de test"],
        ["Esquisse", "le solveur de contraintes PlaneGCS résout un cercle de test"],
        ["Dossiers", "les dossiers de récupération et de leçons sont accessibles en écriture"],
        ["IA (MCP)", "le canal de pilotage s'ouvre et répond ; Python et le module `mcp` sont installés ; `mcp_server.py` est à côté de `silex.exe`"],
    ], [3.4, 13.6])
    m.box("tip", "Le `| Out-Host` est nécessaire parce que `silex.exe` est une application fenêtrée : sans lui, "
                 "PowerShell n'attend pas la fin du rapport.")

    m.h2("4.41 À propos et licences")
    m.p("Menu **?** > **About Silex / Licenses** : version, identifiant de compilation (commit Git), versions de Qt et "
        "d'OpenCascade, et le texte de toutes les licences.")
    m.bullets([
        "Silex est un logiciel libre sous **licence MIT** (« The Silex contributors ») : tu peux l'utiliser, le copier, le "
        "modifier et le redistribuer, y compris dans un cadre commercial, en gardant la mention de licence.",
        "Les bibliothèques utilisées gardent leur propre licence (OpenCascade, Qt, PlaneGCS, FreeType…). La liste complète "
        "est dans `THIRD-PARTY-NOTICES.txt`, à côté de `silex.exe`. La police Liberation Sans est sous licence SIL OFL.",
    ])
    m.page_break()

    # ---------------------------------------------------------------- 5. troubleshooting
    m.h1("5. Dépannage")
    m.table(["Problème", "Solution"], [
        ["La pastille MCP est orange.", "Une autre fenêtre Silex utilise déjà le canal IA. Ferme-la ou lance celle-ci avec `--pipe autre_nom`."],
        ["Silex ne démarre pas (`MSVCP140.dll` manquant).", "Installe le Visual C++ Redistributable x64 (§ 1.3)."],
        ["L'extrusion ne trouve pas de profil.", "Le contour n'est pas fermé : vérifie que chaque extrémité est collée à la suivante (carré jaune à l'accrochage). Un contour fermé apparaît en bleu clair."],
        ["L'esquisse refuse une contrainte.", "Elle rendrait l'esquisse sur-contrainte (deux règles contradictoires). Supprime la cote en trop ou utilise-en une autre."],
        ["Une étape est soulignée en rouge.", "Survole-la pour lire le message (souvent : rayon trop grand, profil qui traverse l'axe…). Double-clique pour corriger : la pièce garde sa dernière forme valide en attendant."],
        ["Une étape est soulignée en orange.", "Une référence a été « re-trouvée » par sa géométrie. Vérifie que le congé ou l'esquisse est au bon endroit."],
        ["Mon fichier STEP n'apparaît pas dans « Ouvrir ».", "Normal : « Ouvrir » ne lit que les `.silex`. Utilise **Importer STEP** (`Ctrl+I`)."],
        ["La barre d'état indique « IPC disabled ».", "Une autre instance de Silex utilise déjà le canal de pilotage. Ferme-la, ou lance celle-ci avec `--pipe autre_nom`."],
        ["« … is a locked reference component »", "L'étape vise une carte de la bibliothèque (🔒). Choisis ta pièce comme cible : on ne modifie pas un composant de référence."],
        ["L'assistant IA répond « Tutor mode ».", "Le mode tuteur est actif : l'IA guide sans modéliser. Désactive « Tuteur IA » dans le panneau Learn pour qu'elle puisse dessiner."],
        ["Des leçons, exemples ou composants manquent, ou l'IA ne se connecte pas.", "Lance `.\\silex.exe --doctor | Out-Host` (§ 4.40) : chaque ligne **[FAIL]** dit quoi réparer. Pour l'IA, lance aussi `python mcp_server.py --check`."],
        ["Silex a planté : ai-je perdu mon travail ?", "Relance Silex : il propose de **restaurer** la dernière sauvegarde automatique (au plus 2 minutes de travail perdues). Enregistre ensuite avec `Ctrl+S` (§ 4.39)."],
        ["Le texte en relief est orange (avertissement).", "Ses traits sont plus fins que la buse de 0,4 mm. Agrandis le texte ou choisis une police plus grasse (§ 4.36)."],
        ["Le texte affiche une erreur de paramètre.", "Un nom entre accolades n'existe pas, ou une accolade n'est pas fermée. Vérifie dans `Ctrl+P` ; pour afficher une accolade, double-la : `{{`."],
        ["Le SVG importé n'a pas la bonne taille.", "Le fichier est en pixels. Refais l'import avec **Target width** et la largeur voulue en mm (§ 4.37)."],
        ["Le dessin importé ne s'extrude pas.", "Un contour n'est pas fermé dans le fichier d'origine (écart de plus de 0,01 mm). Ferme-le avec une ligne, ou corrige le fichier dans Inkscape."],
        ["Je ne vois plus l'image de référence.", "Elle n'apparaît que pendant l'édition de l'esquisse : double-clique l'esquisse dans la timeline. Vérifie aussi le curseur d'opacité."],
    ], [6.2, 10.8])

    # ---------------------------------------------------------------- 6. cheat sheet
    m.h1("6. Aide-mémoire des raccourcis")
    m.table(["Contexte", "Raccourcis"], [
        ["Modélisation directe", "`Q` pousser / tirer · `Maj+B` boîte · `Maj+Y` cylindre · ViewCube : clic face / arête / coin, Maison"],
        ["Corps & faces (v0.4)", "`Suppr` supprimer corps / face (étape annulable) · `V` masquer / montrer · clic droit : Isolate, Move / Copy, Align, Split body"],
        ["Apprendre (v0.4)", "`F1` mode guidé · Expliquer (esquisse) · Hint · Guided tour"],
        ["Makers (v0.5)", "`Maj+H` perçage / insert · Create ▾ : Insert Component, Enclosure Wizard · Modify ▾ : Lay Flat on Bed · Bed (barre d'état) · Learn : défis, Créer une leçon, Tuteur IA · bouton de profil Essentiel / Complet"],
        ["Trio Makers (v0.7)", "`Maj+T` texte en relief / gravé (hors esquisse) · `{paramètre}` dans le texte · esquisse : `Ctrl+Maj+I` importer SVG / DXF, bouton Image, clic droit > Calibrate image"],
        ["Fiabilité (v0.7)", "menu **?** : About / Licenses, Autosave (1 à 15 min), dossier de récupération · `silex --doctor` · `--recover <fichier>`"],
        ["Sélection", "glisser dans le vide : → fenêtre (entièrement dedans) · ← capture (touché) · Ctrl+A (esquisse) · N vue normale (esquisse) · clic droit : contraintes compatibles"],
        ["Modèle", "`S` esquisse · `E` extrusion · `R` révolution · `F` congé · `C` chanfrein · `H` coque · `B` combiner · `M` symétrie · `Maj+P` répétition linéaire · `Maj+C` répétition circulaire · `Maj+W` balayage · `Maj+L` lissage · `W` cadrer · `0 1 3 7` vues"],
        ["Analyse (v0.3)", "`I` mesure · `Maj+I` interférences · `Alt+S` vue en coupe · `Ctrl+P` paramètres · `Ctrl+D` mise en plan"],
        ["Esquisse", "`L` ligne · `R` rectangle · `Maj+R` rectangle par centre · `C` cercle · `A` arc · `U` oblong · `D` cote · `E` projeter · `X` construction · `Suppr` effacer · `Entrée` terminer"],
        ["Édition 2D", "`T` ajuster · `Maj+F` congé · `Maj+O` décaler · valeur + `Tab` / `Entrée` pendant le tracé · `Alt` ignore la grille"],
        ["Contraintes", "`H` horizontal · `V` vertical · `I` coïncident · `P` parallèle · `K` perpendiculaire · `Maj+T` tangent · `Q` égal · `F` bloquer · `M` milieu · `S` symétrie · `O` concentrique"],
        ["Fichier", "`Ctrl+N` nouveau · `Ctrl+O` ouvrir · `Ctrl+S` enregistrer · `Ctrl+I` importer STEP · `Ctrl+E` STEP · `Ctrl+Maj+E` STL · `Ctrl+3` 3MF"],
        ["Édition", "`Ctrl+Z` annuler · `Ctrl+Y` rétablir · `Entrée` valider · `Échap` annuler / désélectionner"],
        ["Souris", "Molette : zoom · Milieu : panoramique · `Maj`+milieu ou `Alt`+gauche : orbite · Clic droit maintenu : menu radial · Double-clic : pièce entière"],
    ], [3.0, 14.0])

    d.save(OUT)
    print("written", OUT)


if __name__ == "__main__":
    build()
