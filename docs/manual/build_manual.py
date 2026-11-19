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
    marks = {1: (150, 78), 2: (520, 78), 3: (250, 250), 4: (1350, 100), 5: (430, 805),
             6: (775, 860), 7: (110, 760), 8: (1330, 905)}
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
    m.p("Version 0.2  ·  Novembre 2026", size=11, color=GREY, align=WD_ALIGN_PARAGRAPH.CENTER)
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
        "Copie le dossier `bin\\Release` (environ 110 Mo) où tu veux, par exemple `C:\\Silex`.",
        "Double-clique sur `silex.exe`.",
        "Si Windows affiche une erreur à propos de `MSVCP140.dll` ou `VCRUNTIME140.dll`, installe le "
        "« Visual C++ Redistributable x64 » de Microsoft (commande : `winget install Microsoft.VCRedist.2015+.x64`).",
    ])
    m.p("Configuration nécessaire : Windows 10 ou 11 en 64 bits et une carte graphique compatible Direct3D 11 "
        "(c'est le cas de presque tous les PC récents).")
    m.box("tip", "Pour ouvrir directement un fichier : `silex.exe monprojet.silex`. Pour voir une pièce de "
                 "démonstration : `silex.exe --demo`.")

    # ---------------------------------------------------------------- 2. basics
    m.h1("2. Prise en main : les fonctions de base")
    m.h2("2.1 L'écran de Silex")
    m.image(overview, "La fenêtre principale (pièce du tutoriel du chapitre 3 chargée)")
    m.table(["#", "Zone", "À quoi ça sert"], [
        ["1", "Barre **Fichier**", "Nouveau, Ouvrir, Enregistrer, Importer STEP, Exporter STEP / STL, Annuler, Rétablir."],
        ["2", "Barre **Modèle**", "Les outils de création : Sketch (esquisse), Extrude, Revolve, Fillet (congé), Chamfer, Shell (coque), Combine, Mirror (symétrie), Linear Pattern et Circular Pattern (répétitions), Fit (cadrer)."],
        ["3", "**Vue 3D**", "Là où tu vois et sélectionnes ta pièce. La grille représente le sol (plan XY)."],
        ["4", "**Inspecteur**", "Le panneau de droite : il montre les réglages de l'outil en cours, ou la liste des corps quand aucun outil n'est actif."],
        ["5", "**Timeline**", "Toutes les étapes de ta pièce, de gauche à droite. Double-clic sur une étape pour la modifier."],
        ["6", "**Marqueur de retour**", "La barre orange : tout ce qui est à sa droite est temporairement désactivé. Glisse-la pour « remonter le temps »."],
        ["7", "**Trièdre**", "Montre l'orientation : X rouge, Y vert, Z bleu (Z = vers le haut)."],
        ["8", "**Barre d'état**", "Nombre de corps, temps de calcul, et le nombre d'erreurs ou d'avertissements s'il y en a."],
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
        ["Tangente", "`T`", "Une ligne et un cercle (ou deux cercles) se touchent sans faire d'angle."],
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
        ["Enregistrer / Enregistrer sous", "`Ctrl+S` / `Ctrl+Maj+S`", "`.silex` : ton projet complet et modifiable (format texte JSON, lisible et versionnable). Silex 0.2 enregistre au format 0.2 et ouvre sans problème les fichiers de la version 0.1."],
        ["Ouvrir", "`Ctrl+O`", "`.silex` uniquement."],
        ["Importer un STEP", "`Ctrl+I`", "`.step` / `.stp` venant d'un autre logiciel ou d'un fabricant (pièce sans historique)."],
        ["Exporter STEP", "`Ctrl+E`", "Format d'échange universel pour les autres logiciels de CAO."],
        ["Exporter STL", "`Ctrl+Maj+E`", "Maillage pour l'impression 3D. Silex demande la précision (0,05 mm par défaut)."],
    ], [4.6, 3.6, 8.8])
    m.box("warn", "Un fichier STEP ou STL ne contient que la forme, pas la recette : pour pouvoir modifier ta pièce "
                  "plus tard, garde toujours le fichier **.silex**.")
    m.page_break()

    # ---------------------------------------------------------------- 3. tutorial
    m.h1("3. Tutoriel pas à pas : l'équerre de fixation")
    m.p("Dans ce tutoriel, tu vas dessiner l'équerre ci-dessous en utilisant les fonctions les plus courantes : "
        "esquisse, cotes, extrusion (nouveau corps, joindre, couper), esquisse sur une face, chanfrein et congé. "
        "Compte environ 20 minutes.")
    m.image(IMG / "plan_reference.png", "Le plan coté de la pièce (vues de dessus, de face et de droite)", width_cm=14)
    m.p("En résumé (en mm) : semelle 40 × 85 × 10, rail avant 70 × 20 × 15 chanfreiné à 5 mm, "
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
        "Installe la passerelle : `python -m pip install --user -r tools\\mcp_bridge\\requirements.txt`.",
        "Déclare le serveur dans Claude Code : `claude mcp add --scope user silex -e SILEX_EXE=<chemin de silex.exe> -- python <chemin>\\tools\\mcp_bridge\\mcp_server.py`.",
    ])
    m.table(["Famille", "Outils"], [
        ["Inspection", "`get_model_tree`, `inspect_body` (volume, validité), `query_topology` (faces et arêtes), `list_solids`, `capture_viewport`"],
        ["Esquisse", "`create_sketch`, `sketch_add_geometry`, `sketch_add_constraints`"],
        ["Opérations", "`extrude`, `revolve`, `fillet_or_chamfer`, `shell`, `combine`, `mirror`, `linear_pattern`, `circular_pattern`"],
        ["Historique", "`update_feature_parameter`, `undo`, `redo`"],
        ["Fichiers", "`import_step`, `export_step`, `export_stl`, `save_design`"],
    ], [3.2, 13.8])
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
    ], [5.8, 11.2])
    m.page_break()

    # ---------------------------------------------------------------- 5. troubleshooting
    m.h1("5. Dépannage")
    m.table(["Problème", "Solution"], [
        ["Silex ne démarre pas (`MSVCP140.dll` manquant).", "Installe le Visual C++ Redistributable x64 (§ 1.3)."],
        ["L'extrusion ne trouve pas de profil.", "Le contour n'est pas fermé : vérifie que chaque extrémité est collée à la suivante (carré jaune à l'accrochage). Un contour fermé apparaît en bleu clair."],
        ["L'esquisse refuse une contrainte.", "Elle rendrait l'esquisse sur-contrainte (deux règles contradictoires). Supprime la cote en trop ou utilise-en une autre."],
        ["Une étape est soulignée en rouge.", "Survole-la pour lire le message (souvent : rayon trop grand, profil qui traverse l'axe…). Double-clique pour corriger : la pièce garde sa dernière forme valide en attendant."],
        ["Une étape est soulignée en orange.", "Une référence a été « re-trouvée » par sa géométrie. Vérifie que le congé ou l'esquisse est au bon endroit."],
        ["Mon fichier STEP n'apparaît pas dans « Ouvrir ».", "Normal : « Ouvrir » ne lit que les `.silex`. Utilise **Importer STEP** (`Ctrl+I`)."],
        ["La barre d'état indique « IPC disabled ».", "Une autre instance de Silex utilise déjà le canal de pilotage. Ferme-la, ou lance celle-ci avec `--pipe autre_nom`."],
    ], [6.2, 10.8])

    # ---------------------------------------------------------------- 6. cheat sheet
    m.h1("6. Aide-mémoire des raccourcis")
    m.table(["Contexte", "Raccourcis"], [
        ["Modèle", "`S` esquisse · `E` extrusion · `R` révolution · `F` congé · `C` chanfrein · `H` coque · `B` combiner · `M` symétrie · `Maj+P` répétition linéaire · `Maj+C` répétition circulaire · `W` cadrer · `0 1 3 7` vues"],
        ["Esquisse", "`L` ligne · `R` rectangle · `C` cercle · `A` arc · `D` cote · `X` construction · `Suppr` effacer · `Entrée` terminer"],
        ["Contraintes", "`H` horizontal · `V` vertical · `I` coïncident · `P` parallèle · `K` perpendiculaire · `T` tangent · `Q` égal · `F` bloquer"],
        ["Fichier", "`Ctrl+N` nouveau · `Ctrl+O` ouvrir · `Ctrl+S` enregistrer · `Ctrl+I` importer STEP · `Ctrl+E` STEP · `Ctrl+Maj+E` STL"],
        ["Édition", "`Ctrl+Z` annuler · `Ctrl+Y` rétablir · `Entrée` valider · `Échap` annuler / désélectionner"],
        ["Souris", "Molette : zoom · Milieu : panoramique · `Maj`+milieu ou `Alt`+gauche : orbite · Clic droit maintenu : menu radial · Double-clic : pièce entière"],
    ], [3.0, 14.0])

    cp = d.core_properties
    cp.author = "Silex"
    cp.last_modified_by = "Silex"
    cp.title = "Silex — Manuel utilisateur"
    cp.subject = "CAO 3D paramétrique"
    d.save(OUT)
    print("written", OUT)


if __name__ == "__main__":
    build()
