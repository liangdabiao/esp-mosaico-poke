#include <string.h>

#include "party.h"

_Static_assert(sizeof(mon_t) == MON_BYTES, "mon_t layout must stay 12 bytes");

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void mon_serialize(const mon_t *m, uint8_t *out)
{
    out[0] = m->species_id;
    out[1] = m->level;
    out[2] = m->hp;
    out[3] = m->intimacy;
    wr16(out + 4, m->explore_value);
    out[6] = m->nickname_idx;
    out[7] = m->flags;
    wr32(out + 8, m->exp);
}

static void mon_deserialize(mon_t *m, const uint8_t *in)
{
    m->species_id = in[0];
    m->level = in[1];
    m->hp = in[2];
    m->intimacy = in[3];
    m->explore_value = rd16(in + 4);
    m->nickname_idx = in[6];
    m->flags = in[7];
    m->exp = rd32(in + 8);
}

void party_init(party_t *p)
{
    if (p) memset(p, 0, sizeof(*p));
}

bool party_better(const mon_t *a, const mon_t *b)
{
    if (!a || !b) return false;
    bool a_shiny = (a->flags & 1u) != 0;
    bool b_shiny = (b->flags & 1u) != 0;
    if (a_shiny != b_shiny) return a_shiny;
    if (a->level != b->level) return a->level > b->level;
    return a->exp > b->exp;
}

bool party_receive(party_t *p, const mon_t *m)
{
    if (!p || !m || m->species_id < 1 || m->species_id > BOX_SPECIES) {
        return false;
    }

    if (p->party_count < PARTY_MAX) {
        p->party[p->party_count++] = *m;
        return true;
    }

    int index = party_box_find(p, m->species_id);
    if (index < 0) {
        index = m->species_id - 1;
        if (p->box[index].species_id) {
            for (index = 0; index < BOX_SPECIES && p->box[index].species_id; index++) {}
            if (index == BOX_SPECIES) return false;
        }
    }
    mon_t *slot = &p->box[index];
    if (slot->species_id == 0 || party_better(m, slot)) *slot = *m;
    return true;
}

bool party_set_leader(party_t *p, uint8_t index)
{
    if (!p || index == 0 || index >= p->party_count) return false;
    mon_t selected = p->party[index];
    memmove(&p->party[1], &p->party[0], (size_t)index * sizeof(mon_t));
    p->party[0] = selected;
    return true;
}

const mon_t *party_leader(const party_t *p)
{
    return (p && p->party_count) ? &p->party[0] : NULL;
}

uint16_t party_total(const party_t *p)
{
    if (!p) return 0;
    uint16_t total = p->party_count;
    for (uint16_t i = 0; i < BOX_SPECIES; i++) {
        if (p->box[i].species_id) total++;
    }
    return total;
}

void party_serialize(const party_t *p, uint8_t *out)
{
    if (!p || !out) return;

    memset(out, 0, PARTY_BYTES);
    uint8_t count = p->party_count > PARTY_MAX ? PARTY_MAX : p->party_count;
    out[0] = count;
    uint8_t box_count = 0;
    for (uint16_t i = 0; i < BOX_SPECIES; i++) {
        if (p->box[i].species_id) box_count++;
    }
    out[1] = box_count;

    uint8_t *party_out = out + 2;
    for (uint8_t i = 0; i < count; i++) {
        mon_serialize(&p->party[i], party_out + (size_t)i * MON_BYTES);
    }

    uint8_t *box_out = party_out + PARTY_MAX * MON_BYTES;
    for (uint16_t i = 0; i < BOX_SPECIES; i++) {
        if (p->box[i].species_id) {
            mon_serialize(&p->box[i], box_out + (size_t)i * MON_BYTES);
        }
    }
}

bool party_deserialize(party_t *p, const uint8_t *in, uint16_t len)
{
    if (!p || !in || len < PARTY_BYTES) return false;

    party_t next;
    party_init(&next);
    next.party_count = in[0] > PARTY_MAX ? PARTY_MAX : in[0];

    const uint8_t *party_in = in + 2;
    for (uint8_t i = 0; i < next.party_count; i++) {
        mon_deserialize(&next.party[i], party_in + (size_t)i * MON_BYTES);
    }

    const uint8_t *box_in = party_in + PARTY_MAX * MON_BYTES;
    for (uint16_t i = 0; i < BOX_SPECIES; i++) {
        mon_t m;
        mon_deserialize(&m, box_in + (size_t)i * MON_BYTES);
        if (m.species_id >= 1 && m.species_id <= BOX_SPECIES) {
            next.box[i] = m;
        }
    }

    *p = next;
    return true;
}

int party_box_find(const party_t *p, uint16_t species) {
 if (!p || species < 1 || species > BOX_SPECIES) return -1;
 for (unsigned i=0;i<BOX_SPECIES;i++) if(p->box[i].species_id==species)return i;
 return -1;
}
int party_box_match(const party_t *p,const mon_t *mon) {
 if(!p||!mon||!mon->species_id)return -1;
 for(unsigned i=0;i<BOX_SPECIES;i++)if(!memcmp(&p->box[i],mon,sizeof(*mon)))return i;
 return -1;
}
bool party_exchange_at(party_t *p,uint8_t slot,uint16_t box_slot) {
 if(!p||slot>=p->party_count||box_slot>=BOX_SPECIES||!p->box[box_slot].species_id)return false;
 mon_t outgoing=p->party[slot];p->party[slot]=p->box[box_slot];p->box[box_slot]=outgoing;
 return true;
}
bool party_exchange(party_t *p,uint8_t slot,uint16_t species) {
 int index=party_box_find(p,species);return index>=0&&party_exchange_at(p,slot,index);
}
