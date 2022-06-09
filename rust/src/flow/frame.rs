use super::uid::UID;
use super::Result;
use crate::flow::file_identifier::FileIdentifier;
use bytes::{Buf, BytesMut};
use num_derive::{FromPrimitive, ToPrimitive};
use num_traits::{FromPrimitive, ToPrimitive};
use std::io::Cursor;
use tokio_util::codec::{Decoder, Encoder};
use xxhash_rust::xxh3;

// TODO:  Figure out what this is set to on the C++ side.
const MAX_FDB_FRAME_LENGTH: u32 = 1024 * 1024;

#[derive(Debug)]
pub struct Frame {
    pub token: UID,
    pub checksum: Option<u64>,
    pub payload: std::vec::Vec<u8>,
}
// // The value does not include the size of `connectPacketLength` itself,
// // but only the other fields of this structure.
// uint32_t connectPacketLength = 0;
// ProtocolVersion protocolVersion; // Expect currentProtocolVersion

// uint16_t canonical_remote_port = 0; // Port number to reconnect to the originating process
// uint64_t connection_id = 0; // Multi-version clients will use the same Id for both connections, other connections
//                            // will set this to zero. Added at protocol Version 0x0FDB00A444020001.

// // IP Address to reconnect to the originating process. Only one of these must be populated.
// uint32_t canonical_remote_ip4 = 0;

// enum connect_packet_flags { FLAG_IPV6 = 1 };
// uint16_t flags = 0;å
// uint8_t canonical_remote_ip6[16] = { 0 };

#[derive(Debug, FromPrimitive, ToPrimitive, PartialEq)]
pub enum ConnectPacketFlags {
    IPV4 = 0,
    IPV6 = 1,
}

#[derive(Debug)]
pub struct ConnectPacket {
    version_flags: u8, // Really just 4 bits
    version: u64,      // protocol version bytes.  Human readable in hex.
    canonical_remote_port: u16,
    connection_id: u64,
    canonical_remote_ip4: u32,
    connect_packet_flags: ConnectPacketFlags, // 16 bits on wire
    canonical_remote_ip6: [u8; 16],
}

impl ConnectPacket {
    pub fn new() -> Self {
        ConnectPacket {
            version_flags: 1, // TODO: set these to real values!
            version: 0xfdb00b072000000,
            canonical_remote_port: 6789,
            connection_id: 1,
            canonical_remote_ip4: 0x7f00_0001,
            connect_packet_flags: ConnectPacketFlags::IPV4,
            canonical_remote_ip6: [0; 16],
        }
    }
    fn as_bytes(&self) -> Vec<u8> {
        let mut vec = Vec::new();
        let len_sz: usize = 4;
        vec.extend_from_slice(&u32::to_le_bytes(0)); // len
        vec.extend_from_slice(&u64::to_le_bytes(
            (self.version_flags as u64) << 60 | self.version,
        ));
        vec.extend_from_slice(&u16::to_le_bytes(self.canonical_remote_port));
        vec.extend_from_slice(&u64::to_le_bytes(self.connection_id));
        vec.extend_from_slice(&u32::to_le_bytes(self.canonical_remote_ip4));
        vec.extend_from_slice(&u16::to_le_bytes(
            self.connect_packet_flags.to_u16().unwrap(),
        ));
        vec.extend_from_slice(&self.canonical_remote_ip6);

        let frame_sz = vec.len();
        vec[0..len_sz].copy_from_slice(&u32::to_le_bytes((frame_sz - len_sz).try_into().unwrap()));
        vec
    }
}

fn get_connect_packet(bytes: &mut BytesMut) -> Result<Option<ConnectPacket>> {
    let cur = Cursor::new(&bytes[..]);
    let start: usize = cur.position().try_into()?;
    let src = &cur.get_ref()[start..];

    let len_sz: usize = 4;
    let version_sz = 8; // note that the 4 msb of the version are flags.

    if src.len() < len_sz + version_sz {
        return Ok(None);
    }

    let len = u32::from_le_bytes(src[0..len_sz].try_into()?);

    if len > MAX_FDB_FRAME_LENGTH {
        return Err("Frame is too long!".into());
    }

    let frame_length = len_sz + len as usize;
    let src = &src[len_sz..(len_sz + (len as usize))];

    let version = u64::from_le_bytes(src[0..version_sz].try_into()?);
    let src = &src[version_sz..];

    let version_flags: u8 = (version >> (60)).try_into()?;
    let version = version & !(0b1111 << 60);

    let canonical_remote_port_sz = 2;
    let canonical_remote_port = u16::from_le_bytes(src[0..canonical_remote_port_sz].try_into()?);
    let src = &src[canonical_remote_port_sz..];

    let connection_id_sz = 8;
    let connection_id = u64::from_le_bytes(src[0..connection_id_sz].try_into()?);
    let src = &src[connection_id_sz..];

    let canonical_remote_ip4_sz = 4;
    let canonical_remote_ip4 = u32::from_le_bytes(src[0..canonical_remote_ip4_sz].try_into()?);
    let src = &src[canonical_remote_ip4_sz..];

    let connect_packet_flags_sz = 2;
    let connect_packet_flags_u16 = u16::from_le_bytes(src[0..connect_packet_flags_sz].try_into()?);
    let connect_packet_flags = ConnectPacketFlags::from_u16(connect_packet_flags_u16)
        .ok_or::<super::Error>("Bad connect_packet_flags".into())?;
    let mut src = &src[connect_packet_flags_sz..];

    let canonical_remote_ip6_sz = 16;
    let canonical_remote_ip6 = if src.len() >= 16 {
        let slice = &src[0..canonical_remote_ip6_sz];
        src = &src[canonical_remote_ip6_sz..];
        slice
    } else {
        &[0; 16]
    };

    let cp = ConnectPacket {
        version_flags,
        canonical_remote_port: canonical_remote_port,
        version,
        connection_id: connection_id,
        canonical_remote_ip4: canonical_remote_ip4,
        connect_packet_flags: connect_packet_flags,
        canonical_remote_ip6: canonical_remote_ip6.try_into()?,
    };

    if src.len() > 0 {
        println!("ConnectPacket: {:x?} (trailing garbage(?): {:?}", cp, src);
    }
    bytes.advance(frame_length);
    Ok(Some(cp))
}

fn get_frame(bytes: &mut BytesMut) -> Result<Option<Frame>> {
    let cur = Cursor::new(&bytes[..]);
    let start: usize = cur.position().try_into()?;
    let src = &cur.get_ref()[start..];
    let len_sz = 4;
    let checksum_sz = 8;
    let uid_sz = 16;

    if src.len() < (len_sz + checksum_sz + uid_sz) {
        return Ok(None);
    }

    let len = u32::from_le_bytes(src[0..len_sz].try_into()?);

    if len > MAX_FDB_FRAME_LENGTH {
        return Err("Frame is too long!".into());
    }

    let len = len as usize;

    let src = &src[len_sz..];
    let frame_length = len_sz + checksum_sz + len;

    let checksum = Some(u64::from_le_bytes(src[0..checksum_sz].try_into()?));
    let src = &src[checksum_sz..];

    if src.len() < len {
        return Ok(None);
    }

    let uid = UID::new(src[0..uid_sz].try_into()?)?;
    let src = &src[uid_sz..];

    // println!("Got {} {:x} {:?} ({:?}) bytes_left={}", len, checksum, uid, uid.get_well_known_endpoint(), src.len());

    let payload = src[0..(len - uid_sz)].to_vec();
    // println!("Payload: {:?}", &src[0..len]);

    bytes.advance(frame_length);

    Ok(Some(Frame {
        token: uid,
        checksum,
        payload,
    }))
}

impl Frame {
    pub fn as_bytes(&self) -> Vec<u8> {
        let len_sz = 4;
        let checksum_sz = 8;
        let uid_sz = 16;

        let mut vec = Vec::<u8>::with_capacity(len_sz + checksum_sz + uid_sz + self.payload.len());

        let len: u32 = (self.payload.len() + 2 * 8).try_into().unwrap();

        if len > MAX_FDB_FRAME_LENGTH {
            println!("Attempt to serialize frame longer than FDB_MAX_FRAME_LENGTH");
            panic!();
        }

        vec.extend_from_slice(&u32::to_le_bytes(len));
        match self.checksum {
            Some(checksum) => vec.extend_from_slice(&u64::to_le_bytes(checksum)),
            None => (),
        };
        vec.extend_from_slice(&u64::to_le_bytes(self.token.uid[0]));
        vec.extend_from_slice(&u64::to_le_bytes(self.token.uid[1]));
        vec.extend_from_slice(&self.payload[..]);

        // println!(
        //     "sent len: {}, vec len: {}, checksum: {}, payload: {:x?} send: {:x?}",
        //     len,
        //     vec.len(),
        //     xxh3_64,
        //     self.payload,
        //     &vec
        // );
        vec
    }
    pub fn peek_file_identifier(&self) -> Result<FileIdentifier> {
        if self.payload.len() < 8 {
            Err(format!(
                "Payload too short to contain file identifier: {:x?}",
                self.payload
            )
            .into())
        } else {
            let file_identifier = u32::from_le_bytes(self.payload[4..8].try_into()?);
            FileIdentifier::new_from_wire(file_identifier)
        }
    }

    fn compute_checksum(&self) -> u64 {
        let mut digest = xxh3::Xxh3::new();
        digest.update(&u64::to_le_bytes(self.token.uid[0]));
        digest.update(&u64::to_le_bytes(self.token.uid[1]));
        digest.update(&self.payload);
        digest.digest()
    }

    pub fn validate(&self) -> Result<()> {
        match self.checksum {
            Some(checksum) => {
                if self.compute_checksum() == checksum {
                    Ok(())
                } else {
                    Err("checksum mismatch".into())
                }
            }
            None => Ok(()),
        }
    }

    // Called new_reply because this populates the checksum field instead of
    // letting some downstream thing validate it.
    pub fn new_reply(token: UID, payload: Vec<u8>) -> Self {
        let mut frame = Self {
            token,
            checksum: None,
            payload,
        };
        frame.checksum = Some(frame.compute_checksum());
        frame
    }
}

pub struct FrameDecoder {
    reading_connect_packet: bool,
}

impl FrameDecoder {
    pub fn new() -> FrameDecoder {
        FrameDecoder {
            reading_connect_packet: true,
        }
    }
}
impl Decoder for FrameDecoder {
    type Item = Frame;
    type Error = super::Error;

    fn decode(&mut self, src: &mut BytesMut) -> super::Result<Option<Frame>> {
        if self.reading_connect_packet {
            if let Some(_) = get_connect_packet(src)? {
                self.reading_connect_packet = false;
            } else {
                return Ok(None);
            }
        }
        get_frame(src)
    }
}

// TODO: The FrameEncoder API is single threaded, in that it wants to append multiple
// frames to a BytesMut.  To use this, response_tx needs to take Frames, not vec<u8>

pub struct FrameEncoder {
    writing_connect_packet: bool,
}

impl FrameEncoder {
    pub fn new() -> FrameEncoder {
        FrameEncoder {
            writing_connect_packet: true,
        }
    }
}
impl Encoder<Frame> for FrameEncoder {
    type Error = super::Error;

    fn encode(&mut self, frame: Frame, dst: &mut BytesMut) -> super::Result<()> {
        if self.writing_connect_packet {
            dst.extend_from_slice(&ConnectPacket::new().as_bytes());
            self.writing_connect_packet = false;
        }
        dst.extend_from_slice(&frame.as_bytes()); // TODO: Remove copies
        Ok(())
    }
}
