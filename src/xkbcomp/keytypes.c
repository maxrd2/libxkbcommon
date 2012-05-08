/************************************************************
 Copyright (c) 1994 by Silicon Graphics Computer Systems, Inc.

 Permission to use, copy, modify, and distribute this
 software and its documentation for any purpose and without
 fee is hereby granted, provided that the above copyright
 notice appear in all copies and that both that copyright
 notice and this permission notice appear in supporting
 documentation, and that the name of Silicon Graphics not be
 used in advertising or publicity pertaining to distribution
 of the software without specific prior written permission.
 Silicon Graphics makes no representation about the suitability
 of this software for any purpose. It is provided "as is"
 without any express or implied warranty.

 SILICON GRAPHICS DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
 SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL SILICON
 GRAPHICS BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL
 DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION  WITH
 THE USE OR PERFORMANCE OF THIS SOFTWARE.

 ********************************************************/

#include "xkbcomp-priv.h"
#include "parseutils.h"
#include "vmod.h"

typedef struct _PreserveInfo
{
    CommonInfo defs;
    short matchingMapIndex;
    unsigned char indexMods;
    unsigned char preMods;
    unsigned short indexVMods;
    unsigned short preVMods;
} PreserveInfo;

#define	_KT_Name	(1<<0)
#define	_KT_Mask	(1<<1)
#define	_KT_Map		(1<<2)
#define	_KT_Preserve	(1<<3)
#define	_KT_LevelNames	(1<<4)

typedef struct _KeyTypeInfo
{
    CommonInfo defs;
    xkb_atom_t name;
    int fileID;
    unsigned mask;
    unsigned vmask;
    bool groupInfo;
    unsigned numLevels;
    unsigned nEntries;
    unsigned szEntries;
    struct xkb_kt_map_entry * entries;
    PreserveInfo *preserve;
    unsigned szNames;
    xkb_atom_t *lvlNames;
} KeyTypeInfo;

typedef struct _KeyTypesInfo
{
    char *name;
    int errorCount;
    int fileID;
    unsigned stdPresent;
    unsigned nTypes;
    KeyTypeInfo *types;
    KeyTypeInfo dflt;
    VModInfo vmods;
} KeyTypesInfo;

static xkb_atom_t tok_ONE_LEVEL;
static xkb_atom_t tok_TWO_LEVEL;
static xkb_atom_t tok_ALPHABETIC;
static xkb_atom_t tok_KEYPAD;

/***====================================================================***/

#define ReportTypeShouldBeArray(t, f) \
    ReportShouldBeArray("key type", (f), TypeTxt(t))
#define ReportTypeBadType(t, f, w) \
    ReportBadType("key type", (f), TypeTxt(t), (w))

/***====================================================================***/

#define MapEntryTxt(x, e) \
    XkbcVModMaskText((x), (e)->mods.real_mods, (e)->mods.vmods)
#define PreserveIndexTxt(x, p) \
    XkbcVModMaskText((x), (p)->indexMods, (p)->indexVMods)
#define PreserveTxt(x, p) \
    XkbcVModMaskText((x), (p)->preMods, (p)->preVMods)
#define TypeTxt(t) \
    XkbcAtomText((t)->name)
#define TypeMaskTxt(t, x) \
    XkbcVModMaskText((x), (t)->mask, (t)->vmask)

/***====================================================================***/

static void
InitKeyTypesInfo(KeyTypesInfo * info, struct xkb_keymap * xkb, KeyTypesInfo * from)
{
    tok_ONE_LEVEL = xkb_intern_atom("ONE_LEVEL");
    tok_TWO_LEVEL = xkb_intern_atom("TWO_LEVEL");
    tok_ALPHABETIC = xkb_intern_atom("ALPHABETIC");
    tok_KEYPAD = xkb_intern_atom("KEYPAD");
    info->name = strdup("default");
    info->errorCount = 0;
    info->stdPresent = 0;
    info->nTypes = 0;
    info->types = NULL;
    info->dflt.defs.defined = 0;
    info->dflt.defs.fileID = 0;
    info->dflt.defs.merge = MergeOverride;
    info->dflt.defs.next = NULL;
    info->dflt.name = XKB_ATOM_NONE;
    info->dflt.mask = 0;
    info->dflt.vmask = 0;
    info->dflt.groupInfo = false;
    info->dflt.numLevels = 1;
    info->dflt.nEntries = info->dflt.szEntries = 0;
    info->dflt.entries = NULL;
    info->dflt.szNames = 0;
    info->dflt.lvlNames = NULL;
    info->dflt.preserve = NULL;
    InitVModInfo(&info->vmods, xkb);
    if (from != NULL)
    {
        info->dflt = from->dflt;
        if (from->dflt.entries)
        {
            info->dflt.entries = uTypedCalloc(from->dflt.szEntries,
                                              struct xkb_kt_map_entry);
            if (info->dflt.entries)
            {
                unsigned sz = from->dflt.nEntries * sizeof(struct xkb_kt_map_entry);
                memcpy(info->dflt.entries, from->dflt.entries, sz);
            }
        }
        if (from->dflt.lvlNames)
        {
            info->dflt.lvlNames = uTypedCalloc(from->dflt.szNames, xkb_atom_t);
            if (info->dflt.lvlNames)
            {
                unsigned sz = from->dflt.szNames * sizeof(xkb_atom_t);
                memcpy(info->dflt.lvlNames, from->dflt.lvlNames, sz);
            }
        }
        if (from->dflt.preserve)
        {
            PreserveInfo *old, *new, *last;
            last = NULL;
            old = from->dflt.preserve;
            for (; old; old = (PreserveInfo *) old->defs.next)
            {
                new = uTypedAlloc(PreserveInfo);
                if (!new)
                    return;
                *new = *old;
                new->defs.next = NULL;
                if (last)
                    last->defs.next = (CommonInfo *) new;
                else
                    info->dflt.preserve = new;
                last = new;
            }
        }
    }
}

static void
FreeKeyTypeInfo(KeyTypeInfo * type)
{
    free(type->entries);
    type->entries = NULL;
    free(type->lvlNames);
    type->lvlNames = NULL;
    if (type->preserve != NULL)
    {
        ClearCommonInfo(&type->preserve->defs);
        type->preserve = NULL;
    }
}

static void
FreeKeyTypesInfo(KeyTypesInfo * info)
{
    free(info->name);
    info->name = NULL;
    if (info->types)
    {
        KeyTypeInfo *type;
        for (type = info->types; type; type = (KeyTypeInfo *) type->defs.next)
        {
            FreeKeyTypeInfo(type);
        }
        info->types = (KeyTypeInfo *) ClearCommonInfo(&info->types->defs);
    }
    FreeKeyTypeInfo(&info->dflt);
}

static KeyTypeInfo *
NextKeyType(KeyTypesInfo * info)
{
    KeyTypeInfo *type;

    type = uTypedAlloc(KeyTypeInfo);
    if (type != NULL)
    {
        memset(type, 0, sizeof(KeyTypeInfo));
        type->defs.fileID = info->fileID;
        info->types = (KeyTypeInfo *) AddCommonInfo(&info->types->defs,
                                                    (CommonInfo *) type);
        info->nTypes++;
    }
    return type;
}

static KeyTypeInfo *
FindMatchingKeyType(KeyTypesInfo * info, KeyTypeInfo * new)
{
    KeyTypeInfo *old;

    for (old = info->types; old; old = (KeyTypeInfo *) old->defs.next)
    {
        if (old->name == new->name)
            return old;
    }
    return NULL;
}

static bool
ReportTypeBadWidth(const char *type, int has, int needs)
{
    ERROR("Key type \"%s\" has %d levels, must have %d\n", type, has, needs);
    ACTION("Illegal type definition ignored\n");
    return false;
}

static bool
AddKeyType(struct xkb_keymap * xkb, KeyTypesInfo * info, KeyTypeInfo * new)
{
    KeyTypeInfo *old;

    if (new->name == tok_ONE_LEVEL)
    {
        if (new->numLevels > 1)
            return ReportTypeBadWidth("ONE_LEVEL", new->numLevels, 1);
        info->stdPresent |= XkbOneLevelMask;
    }
    else if (new->name == tok_TWO_LEVEL)
    {
        if (new->numLevels > 2)
            return ReportTypeBadWidth("TWO_LEVEL", new->numLevels, 2);
        else if (new->numLevels < 2)
            new->numLevels = 2;
        info->stdPresent |= XkbTwoLevelMask;
    }
    else if (new->name == tok_ALPHABETIC)
    {
        if (new->numLevels > 2)
            return ReportTypeBadWidth("ALPHABETIC", new->numLevels, 2);
        else if (new->numLevels < 2)
            new->numLevels = 2;
        info->stdPresent |= XkbAlphabeticMask;
    }
    else if (new->name == tok_KEYPAD)
    {
        if (new->numLevels > 2)
            return ReportTypeBadWidth("KEYPAD", new->numLevels, 2);
        else if (new->numLevels < 2)
            new->numLevels = 2;
        info->stdPresent |= XkbKeypadMask;
    }

    old = FindMatchingKeyType(info, new);
    if (old != NULL)
    {
        bool report;
        if ((new->defs.merge == MergeReplace)
            || (new->defs.merge == MergeOverride))
        {
            KeyTypeInfo *next = (KeyTypeInfo *) old->defs.next;
            if (((old->defs.fileID == new->defs.fileID)
                 && (warningLevel > 0)) || (warningLevel > 9))
            {
                WARN("Multiple definitions of the %s key type\n",
                     XkbcAtomText(new->name));
                ACTION("Earlier definition ignored\n");
            }
            FreeKeyTypeInfo(old);
            *old = *new;
            new->szEntries = new->nEntries = 0;
            new->entries = NULL;
            new->preserve = NULL;
            new->lvlNames = NULL;
            old->defs.next = &next->defs;
            return true;
        }
        report = (old->defs.fileID == new->defs.fileID) && (warningLevel > 0);
        if (report)
        {
            WARN("Multiple definitions of the %s key type\n",
                 XkbcAtomText(new->name));
            ACTION("Later definition ignored\n");
        }
        FreeKeyTypeInfo(new);
        return true;
    }
    old = NextKeyType(info);
    if (old == NULL)
        return false;
    *old = *new;
    old->defs.next = NULL;
    new->nEntries = new->szEntries = 0;
    new->entries = NULL;
    new->szNames = 0;
    new->lvlNames = NULL;
    new->preserve = NULL;
    return true;
}

/***====================================================================***/

static void
MergeIncludedKeyTypes(KeyTypesInfo * into,
                      KeyTypesInfo * from, unsigned merge, struct xkb_keymap * xkb)
{
    KeyTypeInfo *type;

    if (from->errorCount > 0)
    {
        into->errorCount += from->errorCount;
        return;
    }
    if (into->name == NULL)
    {
        into->name = from->name;
        from->name = NULL;
    }
    for (type = from->types; type; type = (KeyTypeInfo *) type->defs.next)
    {
        if (merge != MergeDefault)
            type->defs.merge = merge;
        if (!AddKeyType(xkb, into, type))
            into->errorCount++;
    }
    into->stdPresent |= from->stdPresent;
}

typedef void (*FileHandler) (XkbFile *file, struct xkb_keymap *xkb,
                             unsigned merge, KeyTypesInfo *included);

static bool
HandleIncludeKeyTypes(IncludeStmt * stmt,
                      struct xkb_keymap * xkb, KeyTypesInfo * info, FileHandler hndlr)
{
    unsigned newMerge;
    XkbFile *rtrn;
    KeyTypesInfo included;
    bool haveSelf;

    haveSelf = false;
    if ((stmt->file == NULL) && (stmt->map == NULL))
    {
        haveSelf = true;
        included = *info;
        memset(info, 0, sizeof(KeyTypesInfo));
    }
    else if (ProcessIncludeFile(xkb->context, stmt, XkmTypesIndex, &rtrn,
                                &newMerge))
    {
        InitKeyTypesInfo(&included, xkb, info);
        included.fileID = included.dflt.defs.fileID = rtrn->id;
        included.dflt.defs.merge = newMerge;

        (*hndlr) (rtrn, xkb, newMerge, &included);
        if (stmt->stmt != NULL)
        {
            free(included.name);
            included.name = stmt->stmt;
            stmt->stmt = NULL;
        }
        FreeXKBFile(rtrn);
    }
    else
    {
        info->errorCount += 10;
        return false;
    }
    if ((stmt->next != NULL) && (included.errorCount < 1))
    {
        IncludeStmt *next;
        unsigned op;
        KeyTypesInfo next_incl;

        for (next = stmt->next; next != NULL; next = next->next)
        {
            if ((next->file == NULL) && (next->map == NULL))
            {
                haveSelf = true;
                MergeIncludedKeyTypes(&included, info, next->merge, xkb);
                FreeKeyTypesInfo(info);
            }
            else if (ProcessIncludeFile(xkb->context, next, XkmTypesIndex,
                                        &rtrn, &op))
            {
                InitKeyTypesInfo(&next_incl, xkb, &included);
                next_incl.fileID = next_incl.dflt.defs.fileID = rtrn->id;
                next_incl.dflt.defs.merge = op;
                (*hndlr) (rtrn, xkb, op, &next_incl);
                MergeIncludedKeyTypes(&included, &next_incl, op, xkb);
                FreeKeyTypesInfo(&next_incl);
                FreeXKBFile(rtrn);
            }
            else
            {
                info->errorCount += 10;
                FreeKeyTypesInfo(&included);
                return false;
            }
        }
    }
    if (haveSelf)
        *info = included;
    else
    {
        MergeIncludedKeyTypes(info, &included, newMerge, xkb);
        FreeKeyTypesInfo(&included);
    }
    return (info->errorCount == 0);
}

/***====================================================================***/

static struct xkb_kt_map_entry *
FindMatchingMapEntry(KeyTypeInfo * type, unsigned mask, unsigned vmask)
{
    unsigned int i;
    struct xkb_kt_map_entry * entry;

    for (i = 0, entry = type->entries; i < type->nEntries; i++, entry++)
    {
        if ((entry->mods.real_mods == mask) && (entry->mods.vmods == vmask))
            return entry;
    }
    return NULL;
}

static void
DeleteLevel1MapEntries(KeyTypeInfo * type)
{
    unsigned int i, n;

    for (i = 0; i < type->nEntries; i++)
    {
        if (type->entries[i].level == 0)
        {
            for (n = i; n < type->nEntries - 1; n++)
            {
                type->entries[n] = type->entries[n + 1];
            }
            type->nEntries--;
        }
    }
}

/**
 * Return a pointer to the next free XkbcKTMapEntry, reallocating space if
 * necessary.
 */
static struct xkb_kt_map_entry *
NextMapEntry(KeyTypeInfo * type)
{
    if (type->entries == NULL)
    {
        type->entries = uTypedCalloc(2, struct xkb_kt_map_entry);
        if (type->entries == NULL)
        {
            ERROR("Couldn't allocate map entries for %s\n", TypeTxt(type));
            ACTION("Map entries lost\n");
            return NULL;
        }
        type->szEntries = 2;
        type->nEntries = 0;
    }
    else if (type->nEntries >= type->szEntries)
    {
        type->szEntries *= 2;
        type->entries = uTypedRecalloc(type->entries,
                                       type->nEntries, type->szEntries,
                                       struct xkb_kt_map_entry);
        if (type->entries == NULL)
        {
            ERROR("Couldn't reallocate map entries for %s\n", TypeTxt(type));
            ACTION("Map entries lost\n");
            return NULL;
        }
    }
    return &type->entries[type->nEntries++];
}

static bool
AddPreserve(struct xkb_keymap * xkb,
            KeyTypeInfo * type, PreserveInfo * new, bool clobber, bool report)
{
    PreserveInfo *old;

    old = type->preserve;
    while (old != NULL)
    {
        if ((old->indexMods != new->indexMods) ||
            (old->indexVMods != new->indexVMods))
        {
            old = (PreserveInfo *) old->defs.next;
            continue;
        }
        if ((old->preMods == new->preMods)
            && (old->preVMods == new->preVMods))
        {
            if (warningLevel > 9)
            {
                WARN("Identical definitions for preserve[%s] in %s\n",
                      PreserveIndexTxt(xkb, old), TypeTxt(type));
                ACTION("Ignored\n");
            }
            return true;
        }
        if (report && (warningLevel > 0))
        {
            const char *str;
            WARN("Multiple definitions for preserve[%s] in %s\n",
                  PreserveIndexTxt(xkb, old), TypeTxt(type));

            if (clobber)
                str = PreserveTxt(xkb, new);
            else
                str = PreserveTxt(xkb, old);
            ACTION("Using %s, ", str);
            if (clobber)
                str = PreserveTxt(xkb, old);
            else
                str = PreserveTxt(xkb, new);
            INFO("ignoring %s\n", str);
        }
        if (clobber)
        {
            old->preMods = new->preMods;
            old->preVMods = new->preVMods;
        }
        return true;
    }
    old = uTypedAlloc(PreserveInfo);
    if (!old)
    {
        WSGO("Couldn't allocate preserve in %s\n", TypeTxt(type));
        ACTION("Preserve[%s] lost\n", PreserveIndexTxt(xkb, new));
        return false;
    }
    *old = *new;
    old->matchingMapIndex = -1;
    type->preserve =
        (PreserveInfo *) AddCommonInfo(&type->preserve->defs, &old->defs);
    return true;
}

/**
 * Add a new KTMapEntry to the given key type. If an entry with the same mods
 * already exists, the level is updated (if clobber is TRUE). Otherwise, a new
 * entry is created.
 *
 * @param clobber Overwrite existing entry.
 * @param report true if a warning is to be printed on.
 */
static bool
AddMapEntry(struct xkb_keymap * xkb,
            KeyTypeInfo * type,
            struct xkb_kt_map_entry * new, bool clobber, bool report)
{
    struct xkb_kt_map_entry * old;

    if ((old =
         FindMatchingMapEntry(type, new->mods.real_mods, new->mods.vmods)))
    {
        if (report && (old->level != new->level))
        {
            unsigned use, ignore;
            if (clobber)
            {
                use = new->level + 1;
                ignore = old->level + 1;
            }
            else
            {
                use = old->level + 1;
                ignore = new->level + 1;
            }
            WARN("Multiple map entries for %s in %s\n",
                  MapEntryTxt(xkb, new), TypeTxt(type));
            ACTION("Using %d, ignoring %d\n", use, ignore);
        }
        else if (warningLevel > 9)
        {
            WARN("Multiple occurences of map[%s]= %d in %s\n",
                  MapEntryTxt(xkb, new), new->level + 1, TypeTxt(type));
            ACTION("Ignored\n");
            return true;
        }
        if (clobber)
            old->level = new->level;
        return true;
    }
    if ((old = NextMapEntry(type)) == NULL)
        return false;           /* allocation failure, already reported */
    if (new->level >= type->numLevels)
        type->numLevels = new->level + 1;
    if (new->mods.vmods == 0)
        old->active = true;
    else
        old->active = false;
    old->mods.mask = new->mods.real_mods;
    old->mods.real_mods = new->mods.real_mods;
    old->mods.vmods = new->mods.vmods;
    old->level = new->level;
    return true;
}

static bool
SetMapEntry(KeyTypeInfo * type,
            struct xkb_keymap * xkb, ExprDef * arrayNdx, ExprDef * value)
{
    ExprResult rtrn;
    struct xkb_kt_map_entry entry;

    if (arrayNdx == NULL)
        return ReportTypeShouldBeArray(type, "map entry");
    if (!ExprResolveVModMask(arrayNdx, &rtrn, xkb))
        return ReportTypeBadType(type, "map entry", "modifier mask");
    entry.mods.real_mods = rtrn.uval & 0xff;      /* modifiers < 512 */
    entry.mods.vmods = (rtrn.uval >> 8) & 0xffff; /* modifiers > 512 */
    if ((entry.mods.real_mods & (~type->mask)) ||
        ((entry.mods.vmods & (~type->vmask)) != 0))
    {
        if (warningLevel > 0)
        {
            WARN("Map entry for unused modifiers in %s\n", TypeTxt(type));
            ACTION("Using %s instead of ",
                    XkbcVModMaskText(xkb,
                                    entry.mods.real_mods & type->mask,
                                    entry.mods.vmods & type->vmask));
            INFO("%s\n", MapEntryTxt(xkb, &entry));
        }
        entry.mods.real_mods &= type->mask;
        entry.mods.vmods &= type->vmask;
    }
    if (!ExprResolveLevel(value, &rtrn))
    {
        ERROR("Level specifications in a key type must be integer\n");
        ACTION("Ignoring malformed level specification\n");
        return false;
    }
    entry.level = rtrn.ival - 1;
    return AddMapEntry(xkb, type, &entry, true, true);
}

static bool
SetPreserve(KeyTypeInfo * type,
            struct xkb_keymap * xkb, ExprDef * arrayNdx, ExprDef * value)
{
    ExprResult rtrn;
    PreserveInfo new;

    if (arrayNdx == NULL)
        return ReportTypeShouldBeArray(type, "preserve entry");
    if (!ExprResolveVModMask(arrayNdx, &rtrn, xkb))
        return ReportTypeBadType(type, "preserve entry", "modifier mask");
    new.defs = type->defs;
    new.defs.next = NULL;
    new.indexMods = rtrn.uval & 0xff;
    new.indexVMods = (rtrn.uval >> 8) & 0xffff;
    if ((new.indexMods & (~type->mask)) || (new.indexVMods & (~type->vmask)))
    {
        if (warningLevel > 0)
        {
            WARN("Preserve for modifiers not used by the %s type\n",
                  TypeTxt(type));
            ACTION("Index %s converted to ", PreserveIndexTxt(xkb, &new));
        }
        new.indexMods &= type->mask;
        new.indexVMods &= type->vmask;
        if (warningLevel > 0)
            INFO("%s\n", PreserveIndexTxt(xkb, &new));
    }
    if (!ExprResolveVModMask(value, &rtrn, xkb))
    {
        ERROR("Preserve value in a key type is not a modifier mask\n");
        ACTION("Ignoring preserve[%s] in type %s\n",
                PreserveIndexTxt(xkb, &new), TypeTxt(type));
        return false;
    }
    new.preMods = rtrn.uval & 0xff;
    new.preVMods = (rtrn.uval >> 16) & 0xffff;
    if ((new.preMods & (~new.indexMods))
        || (new.preVMods & (~new.indexVMods)))
    {
        if (warningLevel > 0)
        {
            WARN("Illegal value for preserve[%s] in type %s\n",
                  PreserveTxt(xkb, &new), TypeTxt(type));
            ACTION("Converted %s to ", PreserveIndexTxt(xkb, &new));
        }
        new.preMods &= new.indexMods;
        new.preVMods &= new.indexVMods;
        if (warningLevel > 0)
        {
            INFO("%s\n", PreserveIndexTxt(xkb, &new));
        }
    }
    return AddPreserve(xkb, type, &new, true, true);
}

/***====================================================================***/

static bool
AddLevelName(KeyTypeInfo * type,
             unsigned level, xkb_atom_t name, bool clobber)
{
    if ((type->lvlNames == NULL) || (type->szNames <= level))
    {
        type->lvlNames =
            uTypedRecalloc(type->lvlNames, type->szNames, level + 1, xkb_atom_t);
        if (type->lvlNames == NULL)
        {
            ERROR("Couldn't allocate level names for type %s\n",
                   TypeTxt(type));
            ACTION("Level names lost\n");
            type->szNames = 0;
            return false;
        }
        type->szNames = level + 1;
    }
    else if (type->lvlNames[level] == name)
    {
        if (warningLevel > 9)
        {
            WARN("Duplicate names for level %d of key type %s\n",
                  level + 1, TypeTxt(type));
            ACTION("Ignored\n");
        }
        return true;
    }
    else if (type->lvlNames[level] != XKB_ATOM_NONE)
    {
        if (warningLevel > 0)
        {
            const char *old, *new;
            old = XkbcAtomText(type->lvlNames[level]);
            new = XkbcAtomText(name);
            WARN("Multiple names for level %d of key type %s\n",
                  level + 1, TypeTxt(type));
            if (clobber)
                ACTION("Using %s, ignoring %s\n", new, old);
            else
                ACTION("Using %s, ignoring %s\n", old, new);
        }
        if (!clobber)
            return true;
    }
    if (level >= type->numLevels)
        type->numLevels = level + 1;
    type->lvlNames[level] = name;
    return true;
}

static bool
SetLevelName(KeyTypeInfo * type, ExprDef * arrayNdx, ExprDef * value)
{
    ExprResult rtrn;
    unsigned level;
    xkb_atom_t level_name;

    if (arrayNdx == NULL)
        return ReportTypeShouldBeArray(type, "level name");
    if (!ExprResolveLevel(arrayNdx, &rtrn))
        return ReportTypeBadType(type, "level name", "integer");
    level = rtrn.ival - 1;
    if (!ExprResolveString(value, &rtrn))
    {
        ERROR("Non-string name for level %d in key type %s\n", level + 1,
               XkbcAtomText(type->name));
        ACTION("Ignoring illegal level name definition\n");
        return false;
    }
    level_name = xkb_intern_atom(rtrn.str);
    free(rtrn.str);
    return AddLevelName(type, level, level_name, true);
}

/***====================================================================***/

/**
 * Parses the fields in a type "..." { } description.
 *
 * @param field The field to parse (e.g. modifiers, map, level_name)
 */
static bool
SetKeyTypeField(KeyTypeInfo * type,
                struct xkb_keymap * xkb,
                char *field,
                ExprDef * arrayNdx, ExprDef * value, KeyTypesInfo * info)
{
    ExprResult tmp;

    if (strcasecmp(field, "modifiers") == 0)
    {
        unsigned mods, vmods;
        if (arrayNdx != NULL)
        {
            WARN("The modifiers field of a key type is not an array\n");
            ACTION("Illegal array subscript ignored\n");
        }
        /* get modifier mask for current type */
        if (!ExprResolveVModMask(value, &tmp, xkb))
        {
            ERROR("Key type mask field must be a modifier mask\n");
            ACTION("Key type definition ignored\n");
            return false;
        }
        mods = tmp.uval & 0xff; /* core mods */
        vmods = (tmp.uval >> 8) & 0xffff; /* xkb virtual mods */
        if (type->defs.defined & _KT_Mask)
        {
            WARN("Multiple modifier mask definitions for key type %s\n",
                  XkbcAtomText(type->name));
            ACTION("Using %s, ", TypeMaskTxt(type, xkb));
            INFO("ignoring %s\n", XkbcVModMaskText(xkb, mods, vmods));
            return false;
        }
        type->mask = mods;
        type->vmask = vmods;
        type->defs.defined |= _KT_Mask;
        return true;
    }
    else if (strcasecmp(field, "map") == 0)
    {
        type->defs.defined |= _KT_Map;
        return SetMapEntry(type, xkb, arrayNdx, value);
    }
    else if (strcasecmp(field, "preserve") == 0)
    {
        type->defs.defined |= _KT_Preserve;
        return SetPreserve(type, xkb, arrayNdx, value);
    }
    else if ((strcasecmp(field, "levelname") == 0) ||
             (strcasecmp(field, "level_name") == 0))
    {
        type->defs.defined |= _KT_LevelNames;
        return SetLevelName(type, arrayNdx, value);
    }
    ERROR("Unknown field %s in key type %s\n", field, TypeTxt(type));
    ACTION("Definition ignored\n");
    return false;
}

static bool
HandleKeyTypeVar(VarDef * stmt, struct xkb_keymap * xkb, KeyTypesInfo * info)
{
    ExprResult elem, field;
    ExprDef *arrayNdx;

    if (!ExprResolveLhs(stmt->name, &elem, &field, &arrayNdx))
        return false;           /* internal error, already reported */
    if (elem.str && (strcasecmp(elem.str, "type") == 0))
        return SetKeyTypeField(&info->dflt, xkb, field.str, arrayNdx,
                               stmt->value, info);
    if (elem.str != NULL)
    {
        ERROR("Default for unknown element %s\n", uStringText(elem.str));
        ACTION("Value for field %s ignored\n", uStringText(field.str));
    }
    else if (field.str != NULL)
    {
        ERROR("Default defined for unknown field %s\n",
               uStringText(field.str));
        ACTION("Ignored\n");
    }
    return false;
}

static int
HandleKeyTypeBody(VarDef * def,
                  struct xkb_keymap * xkb, KeyTypeInfo * type, KeyTypesInfo * info)
{
    int ok = 1;
    ExprResult tmp, field;
    ExprDef *arrayNdx;

    for (; def != NULL; def = (VarDef *) def->common.next)
    {
        if ((def->name) && (def->name->type == ExprFieldRef))
        {
            ok = HandleKeyTypeVar(def, xkb, info);
            continue;
        }
        ok = ExprResolveLhs(def->name, &tmp, &field, &arrayNdx);
        if (ok) {
            ok = SetKeyTypeField(type, xkb, field.str, arrayNdx, def->value,
                                 info);
            free(field.str);
        }
    }
    return ok;
}

/**
 * Process a type "XYZ" { } specification in the xkb_types section.
 *
 */
static int
HandleKeyTypeDef(KeyTypeDef * def,
                 struct xkb_keymap * xkb, unsigned merge, KeyTypesInfo * info)
{
    unsigned int i;
    KeyTypeInfo type;

    if (def->merge != MergeDefault)
        merge = def->merge;

    type.defs.defined = 0;
    type.defs.fileID = info->fileID;
    type.defs.merge = merge;
    type.defs.next = NULL;
    type.name = def->name;
    type.mask = info->dflt.mask;
    type.vmask = info->dflt.vmask;
    type.groupInfo = info->dflt.groupInfo;
    type.numLevels = 1;
    type.nEntries = type.szEntries = 0;
    type.entries = NULL;
    type.szNames = 0;
    type.lvlNames = NULL;
    type.preserve = NULL;

    /* Parse the actual content. */
    if (!HandleKeyTypeBody(def->body, xkb, &type, info))
    {
        info->errorCount++;
        return false;
    }

    /* now copy any appropriate map, preserve or level names from the */
    /* default type */
    for (i = 0; i < info->dflt.nEntries; i++)
    {
        struct xkb_kt_map_entry * dflt;
        dflt = &info->dflt.entries[i];
        if (((dflt->mods.real_mods & type.mask) == dflt->mods.real_mods) &&
            ((dflt->mods.vmods & type.vmask) == dflt->mods.vmods))
        {
            AddMapEntry(xkb, &type, dflt, false, false);
        }
    }
    if (info->dflt.preserve)
    {
        PreserveInfo *dflt = info->dflt.preserve;
        while (dflt)
        {
            if (((dflt->indexMods & type.mask) == dflt->indexMods) &&
                ((dflt->indexVMods & type.vmask) == dflt->indexVMods))
            {
                AddPreserve(xkb, &type, dflt, false, false);
            }
            dflt = (PreserveInfo *) dflt->defs.next;
        }
    }
    for (i = 0; i < info->dflt.szNames; i++)
    {
        if ((i < type.numLevels) && (info->dflt.lvlNames[i] != XKB_ATOM_NONE))
        {
            AddLevelName(&type, i, info->dflt.lvlNames[i], false);
        }
    }
    /* Now add the new keytype to the info struct */
    if (!AddKeyType(xkb, info, &type))
    {
        info->errorCount++;
        return false;
    }
    return true;
}

/**
 * Process an xkb_types section.
 *
 * @param file The parsed xkb_types section.
 * @param merge Merge Strategy (e.g. MergeOverride)
 * @param info Pointer to memory where the outcome will be stored.
 */
static void
HandleKeyTypesFile(XkbFile * file,
                   struct xkb_keymap * xkb, unsigned merge, KeyTypesInfo * info)
{
    ParseCommon *stmt;

    free(info->name);
    info->name = uDupString(file->name);
    stmt = file->defs;
    while (stmt)
    {
        switch (stmt->stmtType)
        {
        case StmtInclude:
            if (!HandleIncludeKeyTypes((IncludeStmt *) stmt, xkb, info,
                                       HandleKeyTypesFile))
                info->errorCount++;
            break;
        case StmtKeyTypeDef: /* e.g. type "ONE_LEVEL" */
            if (!HandleKeyTypeDef((KeyTypeDef *) stmt, xkb, merge, info))
                info->errorCount++;
            break;
        case StmtVarDef:
            if (!HandleKeyTypeVar((VarDef *) stmt, xkb, info))
                info->errorCount++;
            break;
        case StmtVModDef: /* virtual_modifiers NumLock, ... */
            if (!HandleVModDef((VModDef *) stmt, xkb, merge, &info->vmods))
                info->errorCount++;
            break;
        case StmtKeyAliasDef:
            ERROR("Key type files may not include other declarations\n");
            ACTION("Ignoring definition of key alias\n");
            info->errorCount++;
            break;
        case StmtKeycodeDef:
            ERROR("Key type files may not include other declarations\n");
            ACTION("Ignoring definition of key name\n");
            info->errorCount++;
            break;
        case StmtInterpDef:
            ERROR("Key type files may not include other declarations\n");
            ACTION("Ignoring definition of symbol interpretation\n");
            info->errorCount++;
            break;
        default:
            WSGO("Unexpected statement type %d in HandleKeyTypesFile\n",
                  stmt->stmtType);
            break;
        }
        stmt = stmt->next;
        if (info->errorCount > 10)
        {
#ifdef NOISY
            ERROR("Too many errors\n");
#endif
            ACTION("Abandoning keytypes file \"%s\"\n", file->topName);
            break;
        }
    }
}

static bool
CopyDefToKeyType(struct xkb_keymap * xkb, struct xkb_key_type * type, KeyTypeInfo * def)
{
    unsigned int i;
    PreserveInfo *pre;

    for (pre = def->preserve; pre != NULL;
         pre = (PreserveInfo *) pre->defs.next)
    {
        struct xkb_kt_map_entry * match;
        struct xkb_kt_map_entry tmp;
        tmp.mods.real_mods = pre->indexMods;
        tmp.mods.vmods = pre->indexVMods;
        tmp.level = 0;
        AddMapEntry(xkb, def, &tmp, false, false);
        match = FindMatchingMapEntry(def, pre->indexMods, pre->indexVMods);
        if (!match)
        {
            WSGO("Couldn't find matching entry for preserve\n");
            ACTION("Aborting\n");
            return false;
        }
        pre->matchingMapIndex = match - def->entries;
    }
    type->mods.real_mods = def->mask;
    type->mods.vmods = def->vmask;
    type->num_levels = def->numLevels;
    type->map_count = def->nEntries;
    type->map = def->entries;
    if (def->preserve)
    {
        type->preserve = uTypedCalloc(type->map_count, struct xkb_mods);
        if (!type->preserve)
        {
            WARN("Couldn't allocate preserve array in CopyDefToKeyType\n");
            ACTION("Preserve setting for type %s lost\n",
                    XkbcAtomText(def->name));
        }
        else
        {
            pre = def->preserve;
            for (; pre != NULL; pre = (PreserveInfo *) pre->defs.next)
            {
                int ndx = pre->matchingMapIndex;
                type->preserve[ndx].mask = pre->preMods;
                type->preserve[ndx].real_mods = pre->preMods;
                type->preserve[ndx].vmods = pre->preVMods;
            }
        }
    }
    else
        type->preserve = NULL;
    type->name = XkbcAtomGetString(def->name);
    if (def->szNames > 0)
    {
        type->level_names = uTypedCalloc(def->numLevels, const char *);

        /* assert def->szNames<=def->numLevels */
        for (i = 0; i < def->szNames; i++)
        {
            type->level_names[i] = XkbcAtomGetString(def->lvlNames[i]);
        }
    }
    else
    {
        type->level_names = NULL;
    }

    def->nEntries = def->szEntries = 0;
    def->entries = NULL;
    return XkbcComputeEffectiveMap(xkb, type, NULL);
}

bool
CompileKeyTypes(XkbFile *file, struct xkb_keymap * xkb, unsigned merge)
{
    unsigned int i;
    struct xkb_key_type *type, *next;
    KeyTypesInfo info;
    KeyTypeInfo *def;

    InitKeyTypesInfo(&info, xkb, NULL);
    info.fileID = file->id;

    HandleKeyTypesFile(file, xkb, merge, &info);

    if (info.errorCount != 0)
        goto err_info;

    i = info.nTypes;
    if ((info.stdPresent & XkbOneLevelMask) == 0)
        i++;
    if ((info.stdPresent & XkbTwoLevelMask) == 0)
        i++;
    if ((info.stdPresent & XkbKeypadMask) == 0)
        i++;
    if ((info.stdPresent & XkbAlphabeticMask) == 0)
        i++;

    if (XkbcAllocClientMap(xkb, XkbKeyTypesMask, i) != Success) {
        WSGO("Couldn't allocate client map\n");
        goto err_info;
    }

    xkb->map->num_types = i;

    if (XkbAllRequiredTypes & (~info.stdPresent)) {
        unsigned missing, keypadVMod;

        missing = XkbAllRequiredTypes & (~info.stdPresent);
        keypadVMod = FindKeypadVMod(xkb);

        if (XkbcInitCanonicalKeyTypes(xkb, missing, keypadVMod) != Success) {
            WSGO("Couldn't initialize canonical key types\n");
            goto err_info;
        }

        if (missing & XkbOneLevelMask)
            xkb->map->types[XkbOneLevelIndex].name =
                XkbcAtomGetString(tok_ONE_LEVEL);
        if (missing & XkbTwoLevelMask)
            xkb->map->types[XkbTwoLevelIndex].name =
                XkbcAtomGetString(tok_TWO_LEVEL);
        if (missing & XkbAlphabeticMask)
            xkb->map->types[XkbAlphabeticIndex].name =
                XkbcAtomGetString(tok_ALPHABETIC);
        if (missing & XkbKeypadMask)
            xkb->map->types[XkbKeypadIndex].name =
                XkbcAtomGetString(tok_KEYPAD);
    }

    next = &xkb->map->types[XkbLastRequiredType + 1];
    for (i = 0, def = info.types; i < info.nTypes; i++) {
        if (def->name == tok_ONE_LEVEL)
            type = &xkb->map->types[XkbOneLevelIndex];
        else if (def->name == tok_TWO_LEVEL)
            type = &xkb->map->types[XkbTwoLevelIndex];
        else if (def->name == tok_ALPHABETIC)
            type = &xkb->map->types[XkbAlphabeticIndex];
        else if (def->name == tok_KEYPAD)
            type = &xkb->map->types[XkbKeypadIndex];
        else
            type = next++;

        DeleteLevel1MapEntries(def);

        if (!CopyDefToKeyType(xkb, type, def))
            goto err_info;

        def = (KeyTypeInfo *)def->defs.next;
    }

    FreeKeyTypesInfo(&info);
    return true;

err_info:
    FreeKeyTypesInfo(&info);
    return false;
}
