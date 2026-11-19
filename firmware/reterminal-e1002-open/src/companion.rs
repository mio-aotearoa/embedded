//! Companion frame codec matching
//! `firmware/reterminal-e1002/companion/airdrop_companion.{h,c}`.
//!
//! The open FoA image transmits these frames on UART1 so an E1002 photoframe
//! (or a host) can consume AWDL/AirDrop session signals over a second link.

pub const AC_MAX_PAYLOAD: usize = 512;
pub const AC_MSG_READY: u8 = 0x01;
pub const AC_MSG_SESSION_OPEN: u8 = 0x02;
pub const AC_MSG_DATA: u8 = 0x03;
pub const AC_MSG_SESSION_CLOSE: u8 = 0x04;
pub const AC_MSG_ERROR: u8 = 0x7f;

const SOF0: u8 = 0xa5;
const SOF1: u8 = 0x5a;

/// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type..payload.
pub fn crc16_ccitt_false(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xffff;
    for &b in data {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            if (crc & 0x8000) != 0 {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    crc
}

/// Encode one companion frame. Returns encoded length, or None if it does not fit.
pub fn encode(ty: u8, seq: u8, payload: &[u8], out: &mut [u8]) -> Option<usize> {
    if payload.len() > AC_MAX_PAYLOAD {
        return None;
    }
    let need = 2 + 1 + 1 + 2 + payload.len() + 2;
    if out.len() < need {
        return None;
    }
    out[0] = SOF0;
    out[1] = SOF1;
    out[2] = ty;
    out[3] = seq;
    out[4] = (payload.len() >> 8) as u8;
    out[5] = (payload.len() & 0xff) as u8;
    out[6..6 + payload.len()].copy_from_slice(payload);
    let crc = crc16_ccitt_false(&out[2..6 + payload.len()]);
    out[6 + payload.len()] = (crc >> 8) as u8;
    out[7 + payload.len()] = (crc & 0xff) as u8;
    Some(need)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ready_frame_matches_c_layout() {
        let mut out = [0u8; 16];
        let n = encode(AC_MSG_READY, 0, &[], &mut out).unwrap();
        assert_eq!(n, 8);
        assert_eq!(&out[..2], &[SOF0, SOF1]);
        assert_eq!(out[2], AC_MSG_READY);
        assert_eq!(out[3], 0);
        assert_eq!(&out[4..6], &[0, 0]);
        let crc = crc16_ccitt_false(&out[2..6]);
        assert_eq!(out[6], (crc >> 8) as u8);
        assert_eq!(out[7], (crc & 0xff) as u8);
    }

    #[test]
    fn session_open_carries_img_id() {
        let mut out = [0u8; 32];
        let n = encode(AC_MSG_SESSION_OPEN, 3, &[0x2a], &mut out).unwrap();
        assert_eq!(n, 9);
        assert_eq!(out[2], AC_MSG_SESSION_OPEN);
        assert_eq!(out[3], 3);
        assert_eq!(out[6], 0x2a);
    }
}
