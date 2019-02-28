/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2018 Haivision Systems Inc.
 * 
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * 
 */


#include <string>
#include <map>
#include <vector>
#include <deque>

#include "fec.h"
#include "core.h"
#include "packet.h"
#include "logging.h"

using namespace std;

bool ParseCorrectorConfig(std::string s, CorrectorConfig& out)
{
    vector<string> parts;
    Split(s, ',', back_inserter(parts));

    // Minimum arguments are: rows,cols.
    if (parts.size() < 2)
        return false;

    out.rows = atoi(parts[0].c_str());
    out.cols = atoi(parts[1].c_str());

    if (out.rows == 0 && parts[0] != "0")
        return false;

    if (out.cols == 0 && parts[1] != "0")
        return false;

    for (vector<string>::iterator i = parts.begin()+2; i != parts.end(); ++i)
    {
        vector<string> keyval;
        Split(*i, ':', back_inserter(keyval));
        if (keyval.size() != 2)
            return false;
        out.parameters[keyval[0]] = keyval[1];
    }

    return true;
}

class DefaultCorrector: public CorrectorBase
{
    CorrectorConfig cfg;
    size_t number_cols;
    size_t number_rows;

public:
    struct Group
    {
        int32_t base; //< Sequence of the first packet in the group
        size_t step;      //< by how many packets the sequence should increase to get the next packet
        size_t drop;      //< by how much the sequence should increase to get to the next series
        size_t collected; //< how many packets were taken to collect the clip

        Group(): base(CSeqNo::m_iMaxSeqNo), step(0), collected(0)
        {
        }

        uint16_t length_clip;
        uint8_t flag_clip;
        uint32_t timestamp_clip;
        vector<char> payload_clip;

        // This is mutable because it's an intermediate buffer for
        // the purpose of output.
        mutable vector<char> output_buffer;

        enum Type
        {
            HORIZ,  // Horizontal, recursive
            VERT,    // Vertical, recursive

            // NOTE: HORIZ/VERT are defined as 0/1 so that not-inversion
            // can flip between them.
            SINGLE  // Horizontal-only with no recursion
        };
    };

    struct RcvGroup: Group
    {
        bool fec;
        RcvGroup(): fec(false) {}
    };

private:

    // Row Groups: every item represents a single row group and collects clips for one row.
    // Col Groups: every item represents a signel column group and collect clips for packets represented in one column

    struct Send
    {
        // We need only ONE horizontal group. Simply after the group
        // is closed (last packet supplied), and the FEC packet extracted,
        // the group is no longer in use.
        Group row;
        vector<Group> cols;
    } snd;

    struct Receive
    {
        SRTSOCKET id;
        bool order_required;

        Receive(): id(SRT_INVALID_SOCK), order_required(false)
        {
        }

        // In reception we need to keep as many horizontal groups as required
        // for possible later tracking. A horizontal group should be dismissed
        // when the size of this container exceeds the `number_rows` (size of the column).
        //
        // The 'deque' type is used here for a trial implementation. A desired solution
        // would be a kind of a ring buffer where new groups are added and old (exceeding
        // the size) automatically dismissed.
        deque<RcvGroup> rowq;

        // Base index at the oldest column platform determines
        // the base index of the queue. Meaning, first you need
        // to determnine the column index, where the index 0 is
        // the fistmost element of this queue. After determining
        // the column index, there must be also a second factor
        // deteremined - which column series it is. So, this can
        // start by extracting the base sequence of the element
        // at the index column. This is the series 0. Now, the
        // distance between these two sequences, divided by
        // rowsize*colsize should return %index-in-column,
        // /number-series. The latter multiplied by the row size
        // is the offset between the firstmost column and the
        // searched column.
        deque<RcvGroup> colq;

        // This keeps the value of "packet received or not".
        // The sequence number of the first cell is rowq[0].base.
        // When dropping a row,
        // - the firstmost element of rowq is removed
        // - the length of one row is removed from this vector
        int32_t cell_base;
        deque<bool> cells;

        struct PrivPacket
        {
            uint32_t hdr[CPacket::PH_SIZE];
            char buffer[CPacket::SRT_MAX_PAYLOAD_SIZE];
            size_t length;

            PrivPacket(size_t size): length(size)
            {
                memset(hdr, 0, sizeof(hdr));
            }

            ~PrivPacket()
            {
            }
        };

        mutable vector<PrivPacket> rebuilt;
    } rcv;

    void ConfigureGroup(Group& g, int32_t seqno, size_t gstep, size_t drop);
    template <class Container>
    void ConfigureColumns(Container& which, size_t gsize, size_t gstep, size_t gslip, int32_t isn);

    void ResetGroup(Group& g);

    // Universal
    void ClipData(Group& g, uint16_t length_net, uint8_t kflg,
            uint32_t timestamp_hw, const char* payload, size_t payload_size);
    void ClipPacket(Group& g, const CPacket& pkt);

    // Sending
    bool CheckGroupClose(Group& g, size_t pos, size_t size);
    void PackControl(const Group& g, signed char groupix, CPacket& pkt, int32_t seqno, int kflg);

    // Receiving

    void MarkCellReceived(int32_t seq);
    bool HangHorizontal(const CPacket& pkt, bool fec_ctl, loss_seqs_t& irrecover);
    bool HangVertical(const CPacket& pkt, bool fec_ctl, loss_seqs_t& irrecover);
    void ClipControlPacket(Group& g, const CPacket& pkt);
    void ClipRebuiltPacket(Group& g, Receive::PrivPacket& pkt);
    void RcvRebuild(Group& g, int32_t seqno, Group::Type tp);
    int32_t RcvGetLossSeqHoriz(Group& g);
    int32_t RcvGetLossSeqVert(Group& g);

    void TranslateLossRecords(const set<int32_t> loss, loss_seqs_t& irrecover);
    void RcvCheckDismissColumn(int colgx, loss_seqs_t& irrecover);
    int RcvGetRowGroupIndex(int32_t seq);
    int RcvGetColumnGroupIndex(int32_t seq);
    void InsertRebuilt(vector<CUnit*>& incoming, CUnitQueue* uq);
    void CollectIrrecoverRow(Group& g, loss_seqs_t& irrecover);
    bool CellAt(int32_t seq);

public:

    DefaultCorrector(CUDT* m_parent, CUnitQueue* uq, const std::string& confstr);

    // Sender side

    // This function creates and stores the FEC control packet with
    // a prediction to be immediately sent. This is called in the function
    // that normally is prepared for extracting a data packet from the sender
    // buffer and send it over the channel.
    virtual bool packCorrectionPacket(ref_t<CPacket> r_packet, int32_t seq, int kflg) ATR_OVERRIDE;

    // This is called at the moment when the sender queue decided to pick up
    // a new packet from the scheduled packets. This should be then used to
    // continue filling the group, possibly followed by final calculating the
    // FEC control packet ready to send.
    virtual void feedSource(ref_t<CPacket> r_packet) ATR_OVERRIDE;


    // Receiver side

    // This function is called at the moment when a new data packet has
    // arrived (no matter if subsequent or recovered). The 'state' value
    // defines the configured level of loss state required to send the
    // loss report.
    virtual bool receive(CUnit* unit, ref_t< vector<CUnit*> > r_incoming, ref_t<loss_seqs_t> r_loss_seqs) ATR_OVERRIDE;

    // Configuration
    virtual size_t extraSize() ATR_OVERRIDE
    {
        // This is the size that is needed extra by packets operated by this corrector.
        // It should be subtracted from a current maximum value for SRTO_PAYLOADSIZE

        // The default FEC uses extra space only for FEC/CTL packet.
        // The timestamp clip is placed in the timestamp field in the header.
        // The payload contains:
        // - the length clip
        // - the flag spec
        // - the payload clip
        // The payload clip takes simply the current length of SRTO_PAYLOADSIZE.
        // So extra 4 bytes are needed, 2 for flags, 2 for length clip.

        return 4;
    }
};

DefaultCorrector::DefaultCorrector(CUDT* parent, CUnitQueue* uq, const std::string& confstr): CorrectorBase(parent, uq)
{
    if (!ParseCorrectorConfig(confstr, cfg))
        throw CUDTException(MJ_NOTSUP, MN_INVAL, 0);

    // It is allowed for rows and cols to have negative value,
    // this way it only marks the fact that particular dimension
    // does not form a FEC group (no FEC control packet sent).
    number_cols = abs(cfg.cols);
    number_rows = abs(cfg.rows);

    if (number_cols == 0 || number_rows == 0)
        throw CUDTException(MJ_NOTSUP, MN_INVAL, 0);

    // Some tautology for better explanation
    size_t row_size = number_cols;
    size_t col_size = number_rows;

    // XXX TESTING ONLY: columns not implemented, require col == 1.
    if (col_size != 1)
    {
        LOGC(mglog.Error, log << "TEST MODE ONLY. PLEASE USE NUMBER ROWS = 1");
        throw CUDTException(MJ_NOTSUP, MN_INVAL, 0);
    }

    // Required to store in the header when rebuilding
    rcv.id = m_parent->socketID();

    // Setup the bit matrix, initialize everything with false.

    // Vertical size (y)
    rcv.cells.resize(col_size * row_size);

    // These sequence numbers are both the value of ISN-1 at the moment
    // when the handshake is done. The sender ISN is generated here, the
    // receiver ISN by the peer. Both should be known after the handshake.
    // Later they will be updated as packets are transmitted.

    int32_t snd_isn = CSeqNo::incseq(m_parent->sndSeqNo());
    int32_t rcv_isn = CSeqNo::incseq(m_parent->rcvSeqNo());

    // Alright, now we need to get the ISN from m_parent
    // to extract the sequence number allowing qualification to the group.
    // The base values must be prepared so that feedSource can qualify them.
    // Obtain by m_parent->getSndSeqNo() and m_parent->getRcvSeqNo()

    // SEPARATE FOR SENDING AND RECEIVING!

    // Now, assignment of the groups requires:
    // For row groups, simply the size of the group suffices.
    // For column groups, you need a whole matrix of all sequence
    // numbers that are base sequence numbers for the group.
    // Sequences that belong to this group are:
    // 1. First packet has seq+1 towards the base.
    // 2. Every next packet has this value + the size of the row group.
    // So: group dispatching is:
    //  - get the column number
    //  - extract the group data for that column
    //  - check if the sequence is later than the group base sequence, if not, report no group for the packet
    //  - sanity check, if the seqdiff divided by row size gets 0 remainder
    //  - The result from the above division can't exceed the column size, otherwise
    //    it's another group. The number of currently collected data should be in 'collected'.

    // Now set up the group starting sequences.
    // The very first group in both dimensions will have the value of ISN in particular direction.

    // Set up sender part.
    //
    // Size: rows
    // Step: 1 (next packet in group is 1 past the previous one)
    // Slip: rows (first packet in the next group is distant to first packet in the previous group by 'rows')
    ConfigureGroup(snd.row, snd_isn, 1, row_size);

    // In the beginning we need just one reception group. New reception
    // groups will be created in tact with receiving packets outside this one.
    // The value of rcv.row[0].base will be used as an absolute base for calculating
    // the index of the group for a given received packet.
    rcv.rowq.resize(1);
    ConfigureGroup(rcv.rowq[0], rcv_isn, 1, row_size);

    if (col_size > 1)
    {
        // Size: cols
        // Step: rows (the next packet in the group is one row later)
        // Slip: rows+1 (the first packet in the next group is later by 1 column + one whole row down)
        ConfigureColumns(snd.cols, number_cols, col_size, number_rows+1, snd_isn);
        ConfigureColumns(rcv.colq, number_cols, col_size, number_rows+1, rcv_isn);
    }

    // The bit markers that mark the received/lost packets will be expanded
    // as packets come in.
    rcv.cell_base = rcv_isn;
}

template <class Container>
void DefaultCorrector::ConfigureColumns(Container& which, size_t gsize, size_t gstep, size_t gslip, int32_t isn)
{
    // This is to initialize the first set of groups.

    // which: group vector.
    // gsize: number of packets in one group
    // gstep: seqdiff between two packets consecutive in the group
    // gslip: seqdiff between the first packet in one group and first packet in the next group
    // isn: sequence number of the first packet in the first group

    which.resize(gsize);

    int32_t seqno = isn;
    for (size_t i = 0; i < gsize; ++i)
    {
        ConfigureGroup(which[i], seqno, gstep, gstep * gsize);
        seqno = CSeqNo::incseq(seqno, gslip);
    }
}

void DefaultCorrector::ConfigureGroup(Group& g, int32_t seqno, size_t gstep, size_t drop)
{
    g.base = seqno;
    g.step = gstep;

    // This actually rewrites the size of the group here, but
    // by having this value precalculated we simply close the
    // group by adding this value to the base sequence.
    g.drop = drop;
    g.collected = 0;

    // Now the buffer spaces for clips.
    g.payload_clip.resize(m_parent->OPT_PayloadSize());
    g.length_clip = 0;
    g.flag_clip = 0;
    g.timestamp_clip = 0;

    // Preallocate the buffer that will be used for storing it for
    // the needs of passing the data through the network.
    // This will be filled with zeros initially, which is unnecessary,
    // but it happeens just once after connection.
    g.output_buffer.resize(m_parent->OPT_PayloadSize() + extraSize() + 4);
}


void DefaultCorrector::ResetGroup(Group& g)
{

    int32_t new_seq_base = CSeqNo::incseq(g.base, g.drop);
    g.base = new_seq_base;
    g.collected = 0;

    // This isn't necessary for ConfigureGroup because the
    // vector after resizing is filled with a given value,
    // by default the default value of the type, char(), that is 0.
    g.length_clip = 0;
    g.flag_clip = 0;
    g.timestamp_clip = 0;
    memset(&g.payload_clip[0], 0, g.payload_clip.size());
}

void DefaultCorrector::feedSource(ref_t<CPacket> r_packet)
{
    // Handy aliases.
    size_t col_size = number_rows;
    size_t row_size = number_cols;

    // Hang on the matrix. Find by r_packet.get()->getSeqNo().

    //    (The "absolute base" is the cell 0 in vertical groups)
    int32_t base = snd.row.base;

    // (we are guaranteed that this packet is a data packet, so
    // we don't have to check if this isn't a control packet)
    int baseoff = CSeqNo::seqoff(base, r_packet.get().getSeqNo());

    int horiz_pos = baseoff;

    if (CheckGroupClose(snd.row, horiz_pos, row_size))
    {
        HLOGC(mglog.Debug, log << "FEC:... HORIZ group closed, B=%" << snd.row.base);
    }
    ClipPacket(snd.row, *r_packet);
    snd.row.collected++;

    // Don't do any column feeding if using column size 1
    if (col_size > 1)
    {
        // 1. Get the number of group in both vertical and horizontal groups:
        //    - Vertical: offset towards base (% row size, but with updated Base seq unnecessary)
        // (Just for a case).
        int vert_gx = baseoff % row_size;

        // 2. Define the position of this packet in the group
        //    - Horizontal: offset towards base (of the given group, not absolute!)
        //    - Vertical: (seq-base)/column_size
        int32_t vert_base = snd.cols[vert_gx].base;
        int vert_off = CSeqNo::seqoff(vert_base, r_packet.get().getSeqNo());

        // It MAY HAPPEN that the base is newer than the sequence of the packet.
        // This may normally happen in the beginning period, where the bases
        // set up initially for all columns got the shift, so they are kinda from
        // the future, and "this sequence" is in a group that is already closed.
        // In this case simply can't clip the packet in the column group.

        bool clip_column = vert_off >= 0 && col_size > 1;

        // SANITY: check if the rule applies on the group
        if (vert_off % row_size)
        {
            LOGC(mglog.Fatal, log << "FEC:feedSource: VGroup #" << vert_gx << " base=%" << vert_base
                    << " WRONG with horiz base=%" << base);

            // Do not place it, it would be wrong.
            return;
        }

        int vert_pos = vert_off / row_size;

        HLOGC(mglog.Debug, log << "FEC:feedSource: %" << r_packet.get().getSeqNo()
                << " B:%" << baseoff << " H:*[" << horiz_pos << "] V(B=%" << vert_base
                << ")[" << vert_gx << "][" << vert_pos << "] "
                << ( clip_column ? "" : "<NO-COLUMN-CLIP>")
                << " size=" << r_packet.get().getLength()
                << " TS=" << r_packet.get().getMsgTimeStamp()
                << " !" << BufferStamp(r_packet.get().m_pcData, r_packet.get().getLength()));

        // 3. The group should be check for the necessity of being closed.
        // Note that FEC packet extraction doesn't change the state of the
        // VERTICAL groups (it can be potentially extracted multiple times),
        // only the horizontal in order to mark that the vertical FEC is
        // extracted already. So, anyway, check if the group limit was reached
        // and it wasn't closed.
        // 4. Apply the clip
        // 5. Increase collected.

        if (clip_column)
        {
            if (CheckGroupClose(snd.cols[vert_gx], vert_pos, col_size))
            {
                HLOGC(mglog.Debug, log << "FEC:... VERT group closed, B=%" << snd.cols[vert_gx].base);
            }
            ClipPacket(snd.cols[vert_gx], *r_packet);
            snd.cols[vert_gx].collected++;
        }
        HLOGC(mglog.Debug, log << "FEC collected: H: " << snd.row.collected << " V[" << vert_gx << "]: " << snd.cols[vert_gx].collected);
    }
    else
    {
        // The above logging instruction in case of no columns
        HLOGC(mglog.Debug, log << "FEC:feedSource: %" << r_packet.get().getSeqNo()
                << " B:%" << baseoff << " H:*[" << horiz_pos << "]"
                << " size=" << r_packet.get().getLength()
                << " TS=" << r_packet.get().getMsgTimeStamp()
                << " !" << BufferStamp(r_packet.get().m_pcData, r_packet.get().getLength()));
        HLOGC(mglog.Debug, log << "FEC collected: H: " << snd.row.collected);
    }

}

bool DefaultCorrector::CheckGroupClose(Group& g, size_t pos, size_t size)
{
    if (pos < size)
        return false;

    ResetGroup(g);
    return true;
}

void DefaultCorrector::ClipPacket(Group& g, const CPacket& pkt)
{
    // Both length and timestamp must be taken as NETWORK ORDER
    // before applying the clip.

    uint16_t length_net = htons(pkt.getLength());
    uint8_t kflg = uint8_t(pkt.getMsgCryptoFlags());

    // NOTE: Unlike length, the TIMESTAMP is NOT endian-reordered
    // because it will be written into the TIMESTAMP field in the
    // header, and header is inverted automatically when sending,
    // unlike the contents of the payload, where the length will be written.
    uint32_t timestamp_hw = pkt.getMsgTimeStamp();

    ClipData(g, length_net, kflg, timestamp_hw, pkt.m_pcData, pkt.getLength());

    HLOGC(mglog.Debug, log << "FEC DATA PKT CLIP: " << hex
            << "FLAGS=" << unsigned(kflg) << " LENGTH[ne]=" << (length_net)
            << " TS[he]=" << timestamp_hw
            << " CLIP STATE: FLAGS=" << unsigned(g.flag_clip)
            << " LENGTH[ne]=" << g.length_clip
            << " TS[he]=" << g.timestamp_clip
            << " PL4=" << (*(uint32_t*)&g.payload_clip[0]));
}

// Clipping a control packet does merely the same, just the packet has
// different contents, so it must be differetly interpreted.
void DefaultCorrector::ClipControlPacket(Group& g, const CPacket& pkt)
{
    // Both length and timestamp must be taken as NETWORK ORDER
    // before applying the clip.

    const char* fec_header = pkt.m_pcData;
    const char* payload = fec_header + 4;
    size_t payload_clip_len = pkt.getLength() - 4;

    const uint8_t* flag_clip = (const uint8_t*)(fec_header + 1);
    const uint16_t* length_clip = (const uint16_t*)(fec_header + 2);

    uint32_t timestamp_hw = pkt.getMsgTimeStamp();

    ClipData(g, *length_clip, *flag_clip, timestamp_hw, payload, payload_clip_len);

    HLOGC(mglog.Debug, log << "FEC/CTL CLIP: " << hex
            << "FLAGS=" << unsigned(*flag_clip) << " LENGTH[ne]=" << (*length_clip)
            << " TS[he]=" << timestamp_hw
            << " CLIP STATE: FLAGS=" << unsigned(g.flag_clip)
            << " LENGTH[ne]=" << g.length_clip
            << " TS[he]=" << g.timestamp_clip
            << " PL4=" << (*(uint32_t*)&g.payload_clip[0]));
}

void DefaultCorrector::ClipRebuiltPacket(Group& g, Receive::PrivPacket& pkt)
{
    uint16_t length_net = htons(pkt.length);
    uint8_t kflg = MSGNO_ENCKEYSPEC::unwrap(pkt.hdr[CPacket::PH_MSGNO]);

    // NOTE: Unlike length, the TIMESTAMP is NOT endian-reordered
    // because it will be written into the TIMESTAMP field in the
    // header, and header is inverted automatically when sending,
    // unlike the contents of the payload, where the length will be written.
    uint32_t timestamp_hw = pkt.hdr[CPacket::PH_TIMESTAMP];

    ClipData(g, length_net, kflg, timestamp_hw, pkt.buffer, pkt.length);

    HLOGC(mglog.Debug, log << "FEC REBUILT DATA CLIP: " << hex
            << "FLAGS=" << unsigned(kflg) << " LENGTH[ne]=" << (length_net)
            << " TS[he]=" << timestamp_hw
            << " CLIP STATE: FLAGS=" << unsigned(g.flag_clip)
            << " LENGTH[ne]=" << g.length_clip
            << " TS[he]=" << g.timestamp_clip
            << " PL4=" << (*(uint32_t*)&g.payload_clip[0]));
}

void DefaultCorrector::ClipData(Group& g, uint16_t length_net, uint8_t kflg,
        uint32_t timestamp_hw, const char* payload, size_t payload_size)
{
    g.length_clip = g.length_clip ^ length_net;
    g.flag_clip = g.flag_clip ^ kflg;
    g.timestamp_clip = g.timestamp_clip ^ timestamp_hw;

    // Payload goes "as is".
    for (size_t i = 0; i < payload_size; ++i)
    {
        g.payload_clip[i] = g.payload_clip[i] ^ payload[i];
    }

    // Fill the rest with zeros. When this packet is going to be
    // recovered, the payload extraced from this process will have
    // the maximum lenght, but it will be cut to the right length
    // and these padding 0s taken out.
    for (size_t i = payload_size; i < m_parent->OPT_PayloadSize(); ++i)
        g.payload_clip[i] = g.payload_clip[i] ^ 0;
}

bool DefaultCorrector::packCorrectionPacket(ref_t<CPacket> r_packet, int32_t seq, int kflg)
{
    // If the FEC packet is not yet ready for extraction, do nothing and return false.
    // Check if seq is the last sequence of the group.

    // 1. Check horizontal readiness first.

    // If it's ready for extraction, extract it, and write into the packet.
    //
    // NOTE: seq is the sequence number of the LAST PACKET SENT regularly.
    // This is only about to be shifted forward by 1 to be placed on the
    // data packet. The packet in `r_packet` doesn't have the sequence number
    // installed yet

    // For BOTH vertical and horizontal snd groups:
    // - Check if the "full group" condition is satisfied (all packets from the group are clipped)
    // - If not, simply return false and do nothing
    // - If so, store the current clip state into the referenced packet, give it the 'seq' sequence

    // After packing the FEC packet:
    // - update the base sequence in the group for which it's packed
    // - make sure that pointers are reset to not suggest the packet is ready

    if (snd.row.collected >= number_cols)
    {
        HLOGC(mglog.Debug, log << "FEC/CTL ready for HORIZ group: %" << seq);
        // SHIP THE HORIZONTAL FEC packet.
        PackControl(snd.row, -1, *r_packet, seq, kflg);

        HLOGC(mglog.Debug, log << "...PACKET size=" << r_packet.get().getLength()
                << " TS=" << r_packet.get().getMsgTimeStamp()
                << " !" << BufferStamp(r_packet.get().m_pcData, r_packet.get().getLength()));

        // RESET THE HORIZONTAL GROUP.
        ResetGroup(snd.row);
        return true;
    }

    // Handle the special case of number_rows == 1, which
    // means we don't use columns.
    if (number_rows <= 1)
        return false;

    int offset = CSeqNo::seqoff(snd.row.base, seq);

    // This can actually happen only for the very first sent packet.
    // It looks like "following the last packet from the previous group",
    // however there was no previous group because this is the first packet.
    if (offset < 0)
        return false;

    int vert_gx = (offset + number_cols) % number_cols;
    if (snd.cols[vert_gx].collected >= number_rows)
    {
        HLOGC(mglog.Debug, log << "FEC/CTL ready for VERT group [" << vert_gx << "]: %" << seq);
        // SHIP THE VERTICAL FEC packet.
        PackControl(snd.cols[vert_gx], vert_gx, *r_packet, seq, kflg);

        // RESET THE GROUP THAT WAS SENT
        ResetGroup(snd.cols[vert_gx]);
        return true;
    }

    return false;
}

void DefaultCorrector::PackControl(const Group& g, signed char index, CPacket& pkt, int32_t seq, int kflg)
{
    // Allocate as much space as needed, regardless of the PAYLOADSIZE value.

    static const size_t INDEX_SIZE = 1;

    size_t total_size =
        INDEX_SIZE
        + sizeof(g.flag_clip)
        + sizeof(g.length_clip)
        + g.payload_clip.size();

    // Sanity
#if ENABLE_DEBUG
    if (g.output_buffer.size() < total_size)
    {
        LOGC(mglog.Fatal, log << "OUTPUT BUFFER TOO SMALL!");
        abort();
    }
#endif

    char* out = pkt.m_pcData = &g.output_buffer[0];
    size_t off = 0;
    // Spread the index. This is the index of the payload in the vertical group.
    // For horizontal group this value is always -1.
    out[off++] = index;
    // Flags, currently only the encryption flags
    out[off++] = g.flag_clip;

    // Ok, now the length clip
    memcpy(out+off, &g.length_clip, sizeof g.length_clip);
    off += sizeof g.length_clip;

    // And finally the payload clip
    memcpy(out+off, &g.payload_clip[0], g.payload_clip.size());

    // Ready. Now fill the header and finalize other data.
    pkt.setLength(total_size);

    pkt.m_iTimeStamp = g.timestamp_clip;
    pkt.m_iSeqNo = seq;

    HLOGC(mglog.Debug, log << "FEC: PackControl: hdr("
            << (total_size - g.payload_clip.size()) << "): INDEX="
            << int(index) << " LENGTH[ne]=" << hex << g.length_clip
            << " FLAGS=" << int(g.flag_clip) << " TS=" << g.timestamp_clip
            << " PL(" << dec << g.payload_clip.size() << ")[0-4]=" << hex
            << (*(uint32_t*)&g.payload_clip[0]));

    // This sets only the Packet Boundary flags, while all other things:
    // - Order
    // - Rexmit
    // - Crypto
    // - Message Number
    // will be set to 0/false
    pkt.m_iMsgNo = MSGNO_PACKET_BOUNDARY::wrap(PB_SOLO);

    // ... and then fix only the Crypto flags
    pkt.setMsgCryptoFlags(EncryptionKeySpec(kflg));

    // Don't set the ID, it will be later set for any kind of packet.
    // Write the timestamp clip into the timestamp field.
}

bool DefaultCorrector::receive(CUnit* unit, ref_t< vector<CUnit*> > r_incoming, ref_t<loss_seqs_t> r_loss_seqs)
{
    // XXX TRIAL IMPLEMENTATION for testing the initial statements.
    // This simply moves the packet to the input queue.
    // Just check if the packet is the FEC packet.

    CPacket& rpkt = unit->m_Packet;

    // size_t row_size = number_cols;
    // size_t col_size = number_rows;

    // Add this packet to the group where it belongs.
    // Light up the cell of this packet to mark it received.
    // Check if any of the groups to which the packet belongs
    // have changed the status into RECOVERABLE.
    //
    // The group has RECOVERABLE status when it has FEC
    // packet received and the number of collected packets counts
    // exactly group_size - 1.

    struct IsFec
    {
        bool row;
        bool col;
        signed char colx;
    } isfec = { false, false, -1 };

    if (rpkt.getMsgSeq() == 0)
    {
        // Interpret the first byte of the contents.
        const char* payload = rpkt.m_pcData;
        isfec.colx = payload[0];
        if (isfec.colx == -1)
        {
            isfec.row = true;
        }
        else
        {
            isfec.col = true;
        }

        HLOGC(mglog.Debug, log << "FEC: msgno=0, FEC/CTL packet detected. INDEX=" << int(payload[0]));
    }

    // Remember this simply every time a packet comes in. In live mode usually
    // this flag is ORD_RELAXED (false), but some earlier versions used ORD_REQUIRED.
    // Even though this flag is now usually ORD_RELAXED, it's fate in live mode
    // isn't completely decided yet, so stay flexible. We believe at least that this
    // flag will stay unchanged during whole connection.
    rcv.order_required = rpkt.getMsgOrderFlag();

    loss_seqs_t irrecover_row, irrecover_col;

    bool ok = true;
    if (!isfec.col) // == regular packet or FEC/ROW
    {
        // Don't manage this packet for horizontal group,
        // if it was a vertical FEC/CTL packet.
        ok = HangHorizontal(rpkt, isfec.row, irrecover_row);
    }

    if (!isfec.row) // == regular packet or FEC/COL
    {
        ok = HangVertical(rpkt, isfec.colx, irrecover_col);
    }

    // Pack first recovered packets, if any.
    if (!rcv.rebuilt.empty())
    {
        InsertRebuilt(*r_incoming, m_unitqueue);
    }

    if (!isfec.col && !isfec.row)
        r_incoming.get().push_back(unit);

    if (!ok)
        return true; // something's wrong, can't FEC-rebuild, manage this yourself

    // For now, report immediately the irrecoverable packets
    // from the row.

    // Later, the `irrecover_row` or `irrecover_col` will be
    // reported only, depending on level settings. For example,
    // with default LATELY level, packets will be reported as
    // irrecoverable only when they are irrecoverable in the
    // vertical group.
    *r_loss_seqs = irrecover_row;

    // The return value should depend on the value of the required level:
    // 0. on ALWAYS, simply return always true.
    // 1. on EARLY, report those that are in irrecover_row, but return false.
    // 2. on LATELY, report those that are in both irrecover, but return false.
    // 3. on NEVER, report nothing and return false.

    // XXX TESTING MODE. Always check for retransmission.
    // Later it should be changed to: return false always,
    // but in r_loss_seqs report sequence numbers of packets
    // that are lost and cannot be recovered at the current
    // recovery level.
    return true;


    // Get the packet from the incoming stream, already recognized
    // as data packet, and then:
    //
    // (Note that the default builtin FEC mechanism uses such rules:
    //  - allows SRT to get the packet, even if it follows the loss
    //  - depending on m_fallback_level, confirms or denies the need that SRT handle the loss
    //  - in loss_seqs we return those that are not recoverable at the current level
    //  - FEC has no extra header provided, so regular data are passed as is
    //)
    // So, the needs to implement:
    // 
    // 1. If this is a FEC packet, close the group, check for lost packets, try to recover.
    // Check if there is recovery possible, if so, request a new unit and pack the recovered packet there.
    // Report the loss to be reported by SRT according to m_fallback_level:
    // - FS_ALWAYS: N/A for a FEC packet
    // - FS_EARLY: When Horizontal group is closed and the packet is not recoverable, report this in loss_seqs
    // - FS_LATELY: When Horizontal and Vertical group is closed and the packet is not recoverable, report it.
    // - FS_NEVER: Always return empty loss_seqs
    //
    // 2. If this is a regular packet, use it for building the FEC group.
    // - FS_ALWAYS: always return true and leave loss_seqs empty.
    // - others: return false and return nothing in loss_seqs
}

void DefaultCorrector::CollectIrrecoverRow(Group& g, loss_seqs_t& irrecover)
{
    // XXX ROW ONLY IMPLEMENTATION.

    // Obtain the group's packet shift

    int32_t base = rcv.cell_base;
    int offset = CSeqNo::seqoff(base, g.base);
    if (offset < 0)
    {
        LOGC(mglog.Error, log << "!!!");
        return;
    }

    size_t maxoff = offset + number_cols;
    // Sanity check, if all cells are really filled.
    if (maxoff > rcv.cells.size())
    {
        LOGC(mglog.Error, log << "!!!");
        return;
    }

    bool last = true;
    loss_seqs_t::value_type val;
    for (size_t i = offset; i < maxoff; ++i)
    {
        bool gone = last;
        last = rcv.cells[i];

        if (gone && !last)
        {
            // Switch full -> loss. Store the sequence, as single (for now)
            val.first = val.second = CSeqNo::incseq(base, i);
        }
        else if (last && !gone)
        {
            val.second = CSeqNo::incseq(base, i);
            irrecover.push_back(val);
        }
    }

    // If it happened that 0 cells were until the end, we are
    // sure that we have the val.first set to the first of the loss list
    // and we've reached the end. Otherwise 'last' would be true.
    if (!last)
    {
        val.second = CSeqNo::incseq(base, int(maxoff)-1);
        irrecover.push_back(val);
    }
}


void DefaultCorrector::InsertRebuilt(vector<CUnit*>& incoming, CUnitQueue* uq)
{
    if (rcv.rebuilt.empty())
        return;

    HLOGC(mglog.Debug, log << "FEC: have rebuilt " << rcv.rebuilt.size() << " packets, PROVIDING");

    for (vector<Receive::PrivPacket>::iterator i = rcv.rebuilt.begin();
            i != rcv.rebuilt.end(); ++i)
    {
        CUnit* u = uq->getNextAvailUnit();
        if (!u)
        {
            LOGC(mglog.Error, log << "FEC: LOCAL STORAGE DEPLETED. Can't return rebuilt packets.");
            break;
        }

        CPacket& packet = u->m_Packet;

        memcpy(packet.getHeader(), i->hdr, CPacket::HDR_SIZE);
        memcpy(packet.m_pcData, i->buffer, i->length);
        packet.setLength(i->length);

        incoming.push_back(u);
    }

    rcv.rebuilt.clear();
}

bool DefaultCorrector::HangHorizontal(const CPacket& rpkt, bool isfec, loss_seqs_t& irrecover)
{

    int32_t seq = rpkt.getSeqNo();
    MarkCellReceived(seq);

    int rowx = RcvGetRowGroupIndex(seq);
    if (rowx == -1)
        return false; // can't access any group to rebuild

    RcvGroup& rowg = rcv.rowq[rowx];
    // Clip the packet into the horizontal group.

    // If this was a regular packet, increase the number of collected.
    // If this was a FEC/CTL packet, keep this number, just set the fec flag.
    if (isfec)
    {
        ClipControlPacket(rowg, rpkt);
        rowg.fec = true;
        HLOGC(mglog.Debug, log << "FEC/H: FEC/CTL packet clipped, %" << seq);
    }
    else
    {
        ClipPacket(rowg, rpkt);
        rowg.collected++;
        HLOGC(mglog.Debug, log << "FEC/H: DATA packet clipped, %" << seq << ", received " << rowg.collected << "/" << number_cols);
    }

    if (rowg.fec && rowg.collected == number_cols - 1)
    {
        HLOGC(mglog.Debug, log << "FEC/H: HAVE " << rowg.collected << " collected & FEC; REBUILDING");
        // The group will provide the information for rebuilding.
        // The sequence of the lost packet can be checked in cells.
        // With the condition of 'collected == number_cols - 1', there
        // should be only one lacking packet, so just rely on first found.
        RcvRebuild(rowg, RcvGetLossSeqHoriz(rowg),
                number_rows == 1 ? Group::SINGLE : Group::HORIZ);
    }

    // When there are only rows, dismiss the oldest row when you have
    // collected at least 1 packet in the next group. Do not dismiss
    // any groups here otherwise - all will be decided during column
    // processing.
    if (number_rows == 1)
    {
        // The conditional row dismissal in row-only configuration.
        // In this configuration, cells and rows go hand-in-hand,
        // so you dismiss a row and then the row-length of cells to
        // make them both base sequence number in sync.
        //
        // The condition that should trigger a row dismissal is the following:
        // - there is more than one row right now, and EITHER:
        //   - there are more than two rows
        //   - the second row collected at least half of the size

        if (rcv.rowq.size() > 2
                || (rcv.rowq.size() > 1 && rcv.rowq[1].collected > number_cols/2))
        {
            // This procedure is a row-only row dismissal.
            // When columns are used, rows will be dismissed only together
            // with the last column supporting it.

            CollectIrrecoverRow(rowg, irrecover);

            HLOGC(mglog.Debug, log << "FEC/H: Dismissing one row, starting at %" << rcv.rowq[0].base);
            // Take the oldest row group, and:
            // - delete it
            // - delete from rcv.cells the size of one dow (number_cols)

            rcv.rowq.pop_front();

            // When columns are not used, also dismiss that number of bits.
            // Use safe version
            size_t ersize = min(number_cols, rcv.cells.size());
            rcv.cells.erase(rcv.cells.begin(), rcv.cells.begin() + ersize);
            rcv.cell_base = CSeqNo::incseq(rcv.cell_base, number_cols);
        }
    }

    return true;
}

int32_t DefaultCorrector::RcvGetLossSeqHoriz(Group& g)
{
    int baseoff = CSeqNo::seqoff(rcv.cell_base, g.base);
    if (baseoff < 0)
    {
        LOGC(mglog.Error, log << "FEC: IPE: negative cell offset, cell_base=%" << rcv.cell_base << " Group's base: %" << g.base << " - NOT ATTEMPTING TO REBUILD");
        return -1;
    }

    // This is a row, so start from the first cell for this group
    // and search lineraly for the first loss.

    int offset = -1;

    for (size_t cix = baseoff; cix < baseoff + number_cols; ++cix)
    {
        if (!rcv.cells[cix])
        {
            offset = cix;

            // Find just one. No more that just one shall be found
            // because it was checked earlier that we have collected
            // all but just one packet.
            break;
        }
    }

    if (offset == -1)
    {
        LOGC(mglog.Fatal, log << "FEC: IPE: rebuilding attempt, but no lost packet found");
        return -1; // sanity, shouldn't happen
    }

    // Now that we have an offset towards the first packet in the cells,
    // translate it to the sequence number of the lost packet.
    return CSeqNo::incseq(rcv.cell_base, offset);
}

int32_t DefaultCorrector::RcvGetLossSeqVert(Group& g)
{
    int baseoff = CSeqNo::seqoff(rcv.cell_base, g.base);
    if (baseoff < 0)
    {
        LOGC(mglog.Error, log << "FEC: IPE: negative cell offset, cell_base=%" << rcv.cell_base << " Group's base: %" << g.base << " - NOT ATTEMPTING TO REBUILD");
        return -1;
    }

    // This is a row, so start from the first cell for this group
    // and search lineraly for the first loss.

    int offset = -1;


    for (size_t cix = baseoff; cix < baseoff + number_cols*number_rows; cix += number_rows)
    {
        if (!rcv.cells[cix])
        {
            offset = cix;

            // Find just one. No more that just one shall be found
            // because it was checked earlier that we have collected
            // all but just one packet.
            break;
        }
    }

    if (offset == -1)
    {
        LOGC(mglog.Fatal, log << "FEC: IPE: rebuilding attempt, but no lost packet found");
        return -1; // sanity, shouldn't happen
    }

    // Now that we have an offset towards the first packet in the cells,
    // translate it to the sequence number of the lost packet.
    return CSeqNo::incseq(rcv.cell_base, offset);
}

void DefaultCorrector::RcvRebuild(Group& g, int32_t seqno, Group::Type tp)
{
    if (seqno == -1)
        return;

    uint16_t length_hw = ntohs(g.length_clip);
    if (length_hw > m_parent->OPT_PayloadSize())
    {
        LOGC(mglog.Error, log << "FEC: DECLIPPED length '" << length_hw << "' exceeds payload size. NOT REBUILDING.");
        return;
    }

    // Rebuild the packet
    // (length_hw is automatically converted through PrivPacket constructor)
    rcv.rebuilt.push_back( length_hw );

    Receive::PrivPacket& p = rcv.rebuilt.back();

    p.hdr[CPacket::PH_SEQNO] = seqno;

    // This is for live mode only, for now, so the message
    // number will be always 1, PB_SOLO, INORDER, and flags from clip.
    // The REXMIT flag is set to 1 to fake that the packet was
    // retransmitted. It is necessary because this packet will
    // come out of sequence order, and if such a packet has
    // no rexmit flag set, it's treated as reordered by network,
    // which isn't true here.
    p.hdr[CPacket::PH_MSGNO] = 1
        | MSGNO_PACKET_BOUNDARY::wrap(PB_SOLO)
        | MSGNO_PACKET_INORDER::wrap(rcv.order_required)
        | MSGNO_ENCKEYSPEC::wrap(g.flag_clip)
        | MSGNO_REXMIT::wrap(true)
        ;

    p.hdr[CPacket::PH_TIMESTAMP] = g.timestamp_clip;
    p.hdr[CPacket::PH_ID] = rcv.id;

    // Header ready, now we rebuild the contents
    // First, rebuild the length.

    // Allocate the buffer and assign to a packet.
    // This is only temporary, it will be copied to
    // the target place when needed, with the buffer coming
    // from the unit queue.

    // The payload clip may be longer than length_hw, but it
    // contains only trailing zeros for completion, which are skipped.
    copy(g.payload_clip.begin(), g.payload_clip.end(), p.buffer);

    HLOGC(mglog.Debug, log << "FEC: REBUILT: %" << seqno
            << " msgno=" << MSGNO_SEQ::unwrap(p.hdr[CPacket::PH_MSGNO])
            << " flags=" << PacketMessageFlagStr(p.hdr[CPacket::PH_MSGNO])
            << " TS=" << p.hdr[CPacket::PH_TIMESTAMP] << " ID=" << dec << p.hdr[CPacket::PH_ID]
            << " size=" << length_hw
            << " !" << BufferStamp(p.buffer, p.length));

    // If this is a single request (filled from row and number_cols == 1),
    // do not attempt recursive rebuilding
    if (tp == Group::SINGLE)
        return;

    // Mark this packet received
    MarkCellReceived(seqno);

    // This flips HORIZ/VERT
    Group::Type crosstype = Group::Type(!tp);

    if (crosstype == Group::HORIZ)
    {
        // Find this packet in the horizontal group
        int rowx = RcvGetRowGroupIndex(seqno);
        if (rowx == -1)
            return; // can't access any group to rebuild
        RcvGroup& rowg = rcv.rowq[rowx];

        // Sanity check. It's impossible that the packet was already
        // rebuilt and any attempt to rebuild a lacking packet was made.
        if (rowg.collected > number_cols - 1)
        {
            return;
        }

        // Same as ClipPacket for the incoming packet, just this
        // is extracting the data directly from the rebuilt one.
        ClipRebuiltPacket(rowg, p);
        rowg.collected++;
        HLOGC(mglog.Debug, log << "FEC/H: REBUILT packet clipped, %" << seqno << ", received " << rowg.collected << "/" << number_cols);

        // Similar as by HangHorizontal, just don't collect irrecoverable packets.
        // They are already known when the packets were collected.
        if (rowg.fec && rowg.collected == number_cols - 1)
        {
            HLOGC(mglog.Debug, log << "FEC/H: HAVE " << rowg.collected << " collected & FEC; REBUILDING");
            // The group will provide the information for rebuilding.
            // The sequence of the lost packet can be checked in cells.
            // With the condition of 'collected == number_cols - 1', there
            // should be only one lacking packet, so just rely on first found.

            // NOTE: RECURSIVE CALL.
            RcvRebuild(rowg, RcvGetLossSeqHoriz(rowg), crosstype);
        }
    }
    else // crosstype == Group::VERT
    {
        // Find this packet in the vertical group
        int colx = RcvGetColumnGroupIndex(seqno);
        if (colx == -1)
            return; // can't access any group to rebuild
        RcvGroup& colg = rcv.colq[colx];

        // Sanity check. It's impossible that the packet was already
        // rebuilt and any attempt to rebuild a lacking packet was made.
        if (colg.collected > number_rows - 1)
        {
            return;
        }

        // Same as ClipPacket for the incoming packet, just this
        // is extracting the data directly from the rebuilt one.
        ClipRebuiltPacket(colg, p);
        colg.collected++;
        HLOGC(mglog.Debug, log << "FEC/V: REBUILT packet clipped, %" << seqno << ", received " << colg.collected << "/" << number_rows);

        // Similar as by HangHorizontal, just don't collect irrecoverable packets.
        // They are already known when the packets were collected.
        if (colg.fec && colg.collected == number_rows - 1)
        {
            HLOGC(mglog.Debug, log << "FEC/V: HAVE " << colg.collected << " collected & FEC; REBUILDING");
            // The group will provide the information for rebuilding.
            // The sequence of the lost packet can be checked in cells.
            // With the condition of 'collected == number_rows - 1', there
            // should be only one lacking packet, so just rely on first found.

            // NOTE: RECURSIVE CALL.
            RcvRebuild(colg, RcvGetLossSeqVert(colg), crosstype);
        }
    }

}

int DefaultCorrector::RcvGetRowGroupIndex(int32_t seq)
{
    RcvGroup& head = rcv.rowq[0];
    int32_t base = head.base;

    int offset = CSeqNo::seqoff(base, seq);

    // Discard the packet, if older than base.
    if (offset < 0)
    {
        HLOGC(mglog.Debug, log << "FEC/H: Packet %" << seq << " is in the past, ignoring");
        return -1;
    }

    // Hang in the receiver group first.
    size_t rowx = offset / number_cols;

    // The packet might have come completely out of the blue.
    // The row group container must be prepared to extend
    // itself in order to give place for the packet.

    // First, possibly extend the row container
    if (rowx >= rcv.rowq.size())
    {
        // Check if oversize. Oversize is when the
        // index is > 2*number_cols. If so, shrink
        // the container first.

        if (rowx > number_cols*2)
        {
            LOGC(mglog.Error, log << "FEC/H: OFFSET=" << rowx << " exceeds maximum row container size, SHRINKING");

            rcv.rowq.erase(rcv.rowq.begin(), rcv.rowq.begin() + number_cols);
            rowx -= number_cols;
        }

        // Create and configure next groups.
        size_t old = rcv.rowq.size();

        // First, add the number of groups.
        rcv.rowq.resize(rowx + 1);

        // Starting from old size
        for (size_t i = old; i < rcv.rowq.size(); ++i)
        {
            // Initialize the base for the row group
            int32_t ibase = CSeqNo::incseq(base, i*number_cols);
            ConfigureGroup(rcv.rowq[i], ibase, 1, number_cols);
        }
    }

    return rowx;
}

void DefaultCorrector::MarkCellReceived(int32_t seq)
{
    // Mark the packet as received. This will allow later to
    // determine, which exactly packet is lost and needs rebuilding.
    int cellsize = rcv.cells.size();
    int cell_offset = CSeqNo::seqoff(rcv.cell_base, seq);
    if (cell_offset >= cellsize)
    {
        // Expand the cell container with zeros, excluding the 'cell_offset'.
        // Resize normally up to the required size, just set the lastmost
        // item to true.
        rcv.cells.resize(cell_offset+1, false);
    }
    rcv.cells[cell_offset] = true;
}

bool DefaultCorrector::CellAt(int32_t seq)
{
    int offset = CSeqNo::seqoff(rcv.cell_base, seq);
    if (offset < 0)
    {
        // XXX IPE!
        return true; // fake we have the packet - this is to collect losses only
    }
    if (offset > int(rcv.cells.size()))
    {
        // XXX IPE!
        return true;
    }

    return rcv.cells[offset];
}

bool DefaultCorrector::HangVertical(const CPacket& rpkt, bool fec_ctl, loss_seqs_t& irrecover)
{
    // Now hang the packet in the vertical group

    int32_t seq = rpkt.getSeqNo();

    // Ok, now we have the column index, we know it exists.
    // Apply the packet.

    int colgx = RcvGetColumnGroupIndex(seq);
    if (colgx == -1)
        return false;

    RcvGroup& colg = rcv.colq[colgx];

    if (!fec_ctl)
    {
        // Data packet, clip it as data
        ClipPacket(colg, rpkt);
        colg.collected++;
    }
    else
    {
        ClipControlPacket(colg, rpkt);
        colg.fec = true;
    }

    if (colg.fec && colg.collected == number_rows - 1)
    {
        RcvRebuild(colg, RcvGetLossSeqVert(colg), Group::VERT);
    }

    // Column dismissal takes place under very strictly specified condition,
    // so simply call it in general here. At least it may happen potentially
    // at any time of when a packet has been received.
    RcvCheckDismissColumn(colgx, irrecover);

    // XXX Consider group dismissal.
    // This should happen if the system is certain
    // that a packet can no longer be recovered.

    // NEEDED ONE MORE API FUNCTION: ack(int32_t seq); ?

    return true;
}

void DefaultCorrector::RcvCheckDismissColumn(int colgx, loss_seqs_t& irrecover)
{
    // The column dismissal should happen at the moment when this
    // column group is no longer necessary. This is considered to
    // have happened at the moment when all packets that might fill
    // in the gap in the group, have been already received and the
    // rebuilding attempt was also already tried and for that group
    // this is no longer possible.
    //
    // As there's no possibility to be certain as to whether any packet
    // (either data with particular sequence number or FEC/CTL packet)
    // will be received, and potentially any number of packets in the
    // series might be lost (even though we regard the sequence threshold
    // limit), there must be first considered the maximum number of
    // column groups (this is currently checked at the RcvGetColumnGroupIndex).
    //
    // When the sequence fits in particular limits so that it can spawn
    // a new column group, we have the second stage of checking:
    //
    // For starters, this is the picture that shows a dismissed column group:

    //   0   1   2   3   4
    //  [ ]  '   '   '   '
    //  [ ] [ ]  '   '   '
    //  [ ] [ ]  # < ----------- removed column
    //  [ ] [ ]  #  [A]  '++
    //  [ ]+[ ]  #  [A] [A]+
    //  [.] [ ]+ #  [A] [A]+
    //  #.###.##############  < --- removed row
    //  [.] [.] [B] [A]+[A]+
    //  [.] [.] [B] [B] [A]++
    //  [.]+[.] [B] [B] [B]
    //      [.]+[B] [B] [B]
    //          [B]+[B] [B]
    //            / [B]+[B]
    //          /       [B]++
    // CLOSED---

    // Currently use the simplified version: having the column index,
    // reach the symmetric column from the previous series and dismiss
    // everything from the start of the container up to this one.

    RcvGroup& colg = rcv.colq[colgx];

    // Index in the same column previous series
    int colg_psx = colgx - number_cols;
    if (colg_psx < 0)
        return; // nothing to dismiss, all groups must be still kept

    // We have a column group to dismiss, pick up its LAST SEQUENCE.
    int32_t last_seq = CSeqNo::incseq(rcv.colq[colg_psx].base, (number_rows * (number_cols-1)));
    //
    // Find the row that manages that sequence.
    int rowoff = CSeqNo::seqoff(rcv.rowq[0].base, last_seq);
    int rowx = rowoff / number_cols;

    // Collect all irrecoverable packets in a set (this is the only way
    // to keep the loss records merged).
    set<int32_t> loss;

    for (int i = 0; i <= colg_psx; ++i)
    {
        // Walk through all sequences in every group.
        RcvGroup& pg = rcv.colq[i];
        for (size_t sof = 0; sof < (number_cols * number_rows); sof += number_cols)
        {
            int32_t lseq = CSeqNo::incseq(pg.base, sof);
            if (!CellAt(lseq))
                loss.insert(lseq);
        }
    }

    // Now remove column groups up to the one designated by colg_psx, including.

    // We have a guarantee that colg_psx is an existing element, so +1 won't
    // exceed the container size.
    rcv.colq.erase(rcv.colq.begin(), rcv.colq.begin() + colg_psx + 1);

    // Now collect losses from all rows about to be dismissed.

    for (int i = 0; i <= rowx; ++i)
    {
        // Walk through all sequences in every group.
        RcvGroup& pg = rcv.rowq[i];
        for (size_t sof = 0; sof < number_cols; sof++)
        {
            int32_t lseq = CSeqNo::incseq(pg.base, sof);
            if (!CellAt(lseq))
                loss.insert(lseq);
        }
    }

    // And dismiss all these rows.
    rcv.rowq.erase(rcv.rowq.begin(), rcv.rowq.begin() + rowx + 1);

    // Now all collected lost packets translate into the range list format
    TranslateLossRecords(loss, irrecover);
}

void DefaultCorrector::TranslateLossRecords(const set<int32_t> loss, loss_seqs_t& irrecover)
{
    if (loss.empty())
        return;

    // size() >= 1 granted
    set<int32_t>::iterator i = loss.begin();

    int32_t fi_start = *i;
    int32_t fi_end = fi_start;
    ++i;
    for (; i != loss.end(); ++i)
    {
        int dist = CSeqNo::seqoff(fi_end, *i);
        if (dist == 1)
            ++fi_end;
        else
        {
            // Jumped over some sequences, cut the range.
            irrecover.push_back(make_pair(fi_start, fi_end));
            fi_start = fi_end = *i;
        }
    }

    // And ship the last one
    irrecover.push_back(make_pair(fi_start, fi_end));
}

int DefaultCorrector::RcvGetColumnGroupIndex(int32_t seqno)
{
    // The column is only the column, not yet
    // exactly the index of the column group in the container.
    // It's like:

    //   0   1   2   3   4
    //  [A]  '   '   '   '
    //  [A] [A]  '   '   '
    //  [A] [A] [A]  '   '
    //  [A] [A] [A] [A]  '++
    //  [A]+[A] [A] [A] [A]+
    //  [B] [A]+[A] [A] [A]+
    //  [B] [B] [A]+[A] [A]+
    //  [B] [B] [B] [A]+[A]+
    //  [B] [B] [B] [B] [A]++
    //  [B]+[B] [B] [B] [B]
    //      [B] [B] [B] [B]
    //          [B] [B] [B]
    //              [B] [B]
    //                  [B]
    //
    // The same groups laid out in the container:
    //
    // [A]0 [A]1 [A]2 [A]3 [A]4 [B]0 [B]1 [B]2 [B]3 [B]4

    // This means, vert_gx returns the number for the
    // logical column, but we only know this column,
    // not the exact group that is assigned to this
    // packet. That is, in the above picture, if the
    // vert_gx resulted in 3, we still don't know if
    // this is A3 or B3.
    //
    // To know it, we need to first take the base
    // sequence number for the very first column group
    // in the container. Every next group in this
    // container is shifted by the 'slip' factor,
    // which in this case is number_cols + 1. The base
    // sequence shifted by number_cols*number_rows
    // becomes the sequence number of the next
    // group in the same column.
    //
    // If a single column is dismissed, then the
    // column 1 will become column 0, but the column
    // 1 in the next series will also become column 0.
    // All due to that the very base sequence for
    // all groups will be the one in the first series
    // column 1, now 0.
    //
    // Therefore, once we have the column, let's extract
    // the column base sequence.
    // As we can't count on that packets will surely come to close a group
    // or just a particular sequence number will be received, we simply have
    // to check all past groups at the moment when this packet is received:
    //
    // 1. Take the sequence number and determine both the GROUP INDEX and the
    // COLUMN INDEX of this group.
    //
    // Important thing here is that the column group base is the base sequence
    // number of the very first group, which NEED NOT GO HAND IN HAND with the
    // row group sequence base. The rules are then:
    //
    // OFFSET = DISTANCE( colq[0].base TO seq )
    //
    // COLUMN_INDEX = OFFSET % number_cols
    //
    // COLUMN_BASE = colq[COLUMN_INDEX].base
    //
    // COLUMN_OFFSET = DISTANCE(COLUMN_BASE TO seq)
    //
    // COLUMN_SERIES = COLUMN_OFFSET / (number_cols * number_rows)
    //
    // GROUP_INDEX = COLUMN_INDEX + (COLUMN_SERIES * number_cols)

    int offset = CSeqNo::seqoff(rcv.colq[0].base, seqno);
    if (offset < 0)
        return -1;

    int colx = offset % number_cols;
    int32_t colbase = rcv.colq[colx].base;
    int coloff = CSeqNo::seqoff(colbase, seqno);
    if (coloff < 0)
    {
        // This means that this sequence number predates the earliest
        // sequence number supported by the very first column.
        return -1;
    }

    int colseries = coloff / (number_cols * number_rows);
    size_t colgx = colx + (colseries * number_cols);

    if (colgx >= rcv.colq.size())
    {
        // Check oversize. Dismiss some earlier items if it exceeds the size.
        // before you extend the size enormously.
        if (colgx > number_rows * 2)
        {
            LOGC(mglog.Error, log << "FEC/H: OFFSET=" << colgx << " exceeds maximum col container size, SHRINKING");

            rcv.colq.erase(rcv.colq.begin(), rcv.colq.begin() + number_rows);
            colgx -= number_rows;
        }

        // XXX Make ConfigureColumns() more universal so that
        // it can start with the current container contents and
        // only add new elements until the end of the new container size.
        size_t old = rcv.colq.size();
        rcv.colq.resize(colgx+1);
        int32_t seqno = rcv.colq[old-1].base;
        size_t gsize = number_cols;
        size_t gstep = number_rows;
        size_t gslip = number_rows + 1;

        seqno = CSeqNo::incseq(seqno, gslip);
        for (size_t i = colgx; i < rcv.colq.size(); ++i)
        {
            ConfigureGroup(rcv.colq[i], seqno, gstep, gstep * gsize);
            seqno = CSeqNo::incseq(seqno, gslip);
        }
    }

    return colgx;

    //
    // Even though column groups are arranged in a "staircase", it only means
    // that DISTANCE(colq[0].base TO colq[1].base) == 1 + number_cols, not 1.
    // But still, this DISTANCE(...) % number_cols == 1. So:
    //
    // COLUMN_INDEX = GROUP_INDEX % number_cols
    //
    // What is special here is that, group base sequence numbers with number_cols == 5
    // is, for example, these column groups in order of how they are arranged in
    // the container have their [INDEX] BASE (stating the very first has base == 10):
    //
    // [0] 10, [1] 16, [2] 22, [3] 28, [4] 34, [5] 40, [6] 46
    //
    // Therefore, if we get the sequence number 51, then:
    //
    // OFFSET = 51 - 10 = 41
    //
    // COLUMN_INDEX = 41 % 5[number_cols] == 1
    //
    // COLUMN_BASE = colq[1].base == 16
    //
    // COLUMN_OFFSET = DISTANCE(colq[1].base TO 51) = 51 - 16 = 35
    //
    // COLUMN_SERIES = COLUMN_OFFSET / (number_cols * number_rows) 
    //               = 35 / (5*5) = 35/25 = 1
    //
    // GROUP_INDEX = COLUMN_INDEX + (COLUMN_SERIES * number_cols)
    //             = 1 + 1*5 = 6
    //
    // --> We have identified column group with index 6, this one
    // that is designated above as [6] 46. This column group collects
    // the following sequence numbers:
    // - 46
    // - 51 (the one we were searching)
    // - 56
    // - 61
    // - 66
    //
    // We have then column group with index 6. Now we go backward in the column
    // group container starting from the direct previous column up to either
    // the start of container or the size == number_cols (5), to see if the
    // group "is closed". So:
    //
    // END = min(number_cols, colq.size()) <--- should be 5 or less, if there's less in the container
    //
    // FOR EACH i = 1 TO END:
    //     g = colq[GROUP_INDEX - i]
    //     gs = SHIFT(seq, -i)
    //     gmax = SHIFT(g.base, number_cols * number_rows)
    //     IF ( gs %> gmax )
    //        DISMISS COLUMNS from 0 to GROUP_INDEX - i; break

}

Corrector::NamePtr Corrector::builtin_correctors[] =
{
    {"default", Creator<DefaultCorrector>::Create },
};

bool Corrector::IsBuiltin(const string& s)
{
    size_t size = sizeof builtin_correctors/sizeof(builtin_correctors[0]);
    for (size_t i = 0; i < size; ++i)
        if (s == builtin_correctors[i].first)
            return true;

    return false;
}

Corrector::correctors_map_t Corrector::correctors;

void Corrector::globalInit()
{
    // Add the builtin correctors to the global map.
    // Users may add their correctors after that.
    // This function is called from CUDTUnited::startup,
    // which is guaranteed to run the initializing
    // procedures only once per process.

    for (size_t i = 0; i < sizeof builtin_correctors/sizeof (builtin_correctors[0]); ++i)
        correctors[builtin_correctors[i].first] = builtin_correctors[i].second;

    // Actually there's no problem with calling this function
    // multiple times, at worst it will overwrite existing correctors
    // with the same builtin.
}

bool Corrector::configure(CUDT* m_parent, CUnitQueue* uq, const std::string& confstr)
{
    CorrectorConfig cfg;
    if (!ParseCorrectorConfig(confstr, cfg))
        return false;

    // Extract the "type" key from parameters, or use
    // builtin if lacking.
    string type = cfg.parameters["type"];

    correctors_map_t::iterator selector;
    if ( type == "")
        selector = correctors.begin();
    else
    {
        selector = correctors.find(type);
        if (selector == correctors.end())
            return false;
    }

    // Found a corrector, so call the creation function
    corrector = (*selector->second)(m_parent, uq, confstr);

    // The corrector should have pinned in all events
    // that are of its interest. It's stated that
    // it's ready after creation.
    return !!corrector;
}

bool Corrector::correctConfig(const CorrectorConfig& conf)
{
    const string* pname = map_getp(conf.parameters, "type");

    if (!pname)
        return true; // default, parameters ignored

    if (*pname == "adaptive")
        return true;

    correctors_map_t::iterator x = correctors.find(*pname);
    if (x == correctors.end())
        return false;

    return true;
}

Corrector::~Corrector()
{
    delete corrector;
    corrector = 0;
}