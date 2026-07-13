use librespot::{
    connect::{ConnectConfig, LoadRequest, LoadRequestOptions, PlayingTrack, Spirc},
    core::{
        authentication::Credentials, cache::Cache, config::SessionConfig, session::Session,
        SpotifyUri,
    },
    protocol::playlist4_external::SelectedListContent,
    playback::{
        audio_backend::{Sink, SinkError, SinkResult},
        config::{PlayerConfig, VolumeCtrl},
        convert::Converter,
        decoder::AudioPacket,
        mixer::{self, Mixer, MixerConfig},
        player::{Player, PlayerEvent},
    },
};
use std::{
    cmp,
    collections::VecDeque,
    ffi::CStr,
    os::raw::{c_char, c_void},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Condvar, Mutex,
    },
    time::{Duration, Instant},
};
use protobuf::Message;
use tokio::{runtime::Runtime, task::JoinHandle};

const BRIDGE_VERSION: &[u8] = b"spotiamp-playback-bridge/0.2.0-librespot\0";
const AUDIO_SAMPLE_RATE: usize = 44100;
const AUDIO_CHANNELS: usize = 2;
const AUDIO_BYTES_PER_SAMPLE: usize = 2;
const AUDIO_BYTES_PER_SECOND: usize = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE;
const AUDIO_CAPACITY_BYTES: usize = AUDIO_BYTES_PER_SECOND;

static GLOBAL_BRIDGE: Mutex<Option<Bridge>> = Mutex::new(None);

fn normalize_playlist_uri(uri: &str) -> Option<String> {
    if uri.starts_with("spotify:playlist:") {
        return Some(uri.to_string());
    }
    let marker = ":playlist:";
    let pos = uri.find(marker)?;
    let id_start = pos + marker.len();
    let id = uri[id_start..]
        .split(|c| c == ':' || c == '?' || c == '#')
        .next()
        .unwrap_or("");
    if id.is_empty() {
        None
    } else {
        Some(format!("spotify:playlist:{id}"))
    }
}

fn looks_like_discover_weekly(name: &str, format: &str) -> bool {
    let name = name.to_ascii_lowercase();
    let format = format.to_ascii_lowercase();
    (format.contains("discover") && format.contains("weekly"))
        || (name.contains("discover") && name.contains("weekly"))
}

fn discover_weekly_from_rootlist(rootlist: &SelectedListContent) -> Option<String> {
    let contents = rootlist.contents.get_or_default();
    for (index, item) in contents.items.iter().enumerate() {
        let Some(meta) = contents.meta_items.get(index) else {
            continue;
        };
        let attributes = meta.attributes.get_or_default();
        let name = attributes.name();
        let format = attributes.format();
        if looks_like_discover_weekly(name, format) {
            if let Some(uri) = normalize_playlist_uri(item.uri()) {
                eprintln!(
                    "[spotiamp_playback_bridge] Discover Weekly rootlist match: name='{name}' format='{format}' uri='{uri}'"
                );
                return Some(uri);
            }
        }
    }

    let mut shown = 0;
    for (index, item) in contents.items.iter().enumerate() {
        let Some(meta) = contents.meta_items.get(index) else {
            continue;
        };
        let attributes = meta.attributes.get_or_default();
        let name = attributes.name();
        let format = attributes.format();
        if !name.is_empty() || !format.is_empty() {
            eprintln!(
                "[spotiamp_playback_bridge] rootlist item: name='{name}' format='{format}' uri='{}'",
                item.uri()
            );
            shown += 1;
            if shown >= 8 {
                break;
            }
        }
    }

    None
}

struct AudioPipe {
    state: Mutex<AudioPipeState>,
    readable: Condvar,
    writable: Condvar,
}

struct AudioPipeState {
    buffer: VecDeque<u8>,
    closed: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum BridgePlayState {
    Unknown = 0,
    Playing = 1,
    Paused = 2,
    Loading = 3,
    Stopped = 4,
}

struct PlaybackState {
    inner: Mutex<PlaybackStateInner>,
}

struct PlaybackStateInner {
    command_id: u64,
    event_generation: u64,
    audio_generation: u64,
    play_request_id: u64,
    play_state: BridgePlayState,
    position_ms: u32,
    nominal_position_ms: u32,
    audio_time_remainder: usize,
    updated_at: Instant,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct SpPlaybackSnapshot {
    command_id: u64,
    event_generation: u64,
    audio_generation: u64,
    play_request_id: u64,
    state: i32,
    position_ms: u32,
    nominal_position_ms: u32,
    buffered_ms: u32,
    started_audio: i32,
    loading: i32,
}

impl PlaybackState {
    fn new() -> Self {
        Self {
            inner: Mutex::new(PlaybackStateInner {
                command_id: 0,
                event_generation: 0,
                audio_generation: 0,
                play_request_id: 0,
                play_state: BridgePlayState::Unknown,
                position_ms: 0,
                nominal_position_ms: 0,
                audio_time_remainder: 0,
                updated_at: Instant::now(),
            }),
        }
    }

    fn begin_command(&self, play_state: BridgePlayState, position_ms: u32) -> u64 {
        let mut inner = self.inner.lock().unwrap();
        inner.command_id = inner.command_id.saturating_add(1);
        inner.play_state = play_state;
        inner.position_ms = position_ms;
        inner.nominal_position_ms = position_ms;
        inner.audio_time_remainder = 0;
        inner.updated_at = Instant::now();
        inner.command_id
    }

    fn update(&self, play_state: BridgePlayState, position_ms: u32) {
        let mut inner = self.inner.lock().unwrap();
        inner.event_generation = inner.event_generation.saturating_add(1);
        inner.play_state = play_state;
        inner.position_ms = position_ms;
        inner.nominal_position_ms = position_ms;
        inner.audio_time_remainder = 0;
        inner.updated_at = Instant::now();
    }

    fn update_event(&self, play_state: BridgePlayState, play_request_id: u64, position_ms: u32) {
        let mut inner = self.inner.lock().unwrap();
        inner.event_generation = inner.event_generation.saturating_add(1);
        inner.play_request_id = play_request_id;
        inner.play_state = play_state;
        inner.position_ms = position_ms;
        inner.nominal_position_ms = position_ms;
        inner.audio_time_remainder = 0;
        inner.updated_at = Instant::now();
    }

    fn set_play_state(&self, play_state: BridgePlayState, play_request_id: u64, position_ms: u32) {
        let mut inner = self.inner.lock().unwrap();
        inner.play_state = play_state;
        inner.play_request_id = play_request_id;
        inner.nominal_position_ms = position_ms;
        inner.updated_at = Instant::now();
    }

    fn force_play_state(&self, play_state: BridgePlayState) {
        let mut inner = self.inner.lock().unwrap();
        inner.play_state = play_state;
        inner.updated_at = Instant::now();
    }

    fn play_state(&self) -> BridgePlayState {
        self.inner.lock().unwrap().play_state
    }

    fn position_ms(&self) -> u32 {
        self.inner.lock().unwrap().position_ms
    }

    fn advance_audio_bytes(&self, bytes: usize) {
        if bytes == 0 {
            return;
        }
        let mut inner = self.inner.lock().unwrap();
        if inner.play_state == BridgePlayState::Playing
            || inner.play_state == BridgePlayState::Loading
        {
            let total = inner.audio_time_remainder + bytes * 1000;
            let elapsed_ms = (total / AUDIO_BYTES_PER_SECOND) as u32;
            inner.audio_time_remainder = total % AUDIO_BYTES_PER_SECOND;
            inner.position_ms = inner.position_ms.saturating_add(elapsed_ms);
            inner.play_state = BridgePlayState::Playing;
            if inner.audio_generation < inner.event_generation {
                inner.audio_generation = inner.event_generation;
            }
            inner.updated_at = Instant::now();
        }
    }

    fn snapshot(&self, buffered_bytes: usize) -> SpPlaybackSnapshot {
        let inner = self.inner.lock().unwrap();
        SpPlaybackSnapshot {
            command_id: inner.command_id,
            event_generation: inner.event_generation,
            audio_generation: inner.audio_generation,
            play_request_id: inner.play_request_id,
            state: inner.play_state as i32,
            position_ms: inner.position_ms,
            nominal_position_ms: inner.nominal_position_ms,
            buffered_ms: (buffered_bytes.saturating_mul(1000) / AUDIO_BYTES_PER_SECOND) as u32,
            started_audio: (inner.audio_generation >= inner.event_generation
                && inner.event_generation > 0) as i32,
            loading: (inner.play_state == BridgePlayState::Loading) as i32,
        }
    }
}

impl AudioPipe {
    fn new() -> Self {
        Self {
            state: Mutex::new(AudioPipeState {
                buffer: VecDeque::with_capacity(AUDIO_CAPACITY_BYTES),
                closed: false,
            }),
            readable: Condvar::new(),
            writable: Condvar::new(),
        }
    }

    fn close(&self) {
        let mut state = self.state.lock().unwrap();
        state.closed = true;
        state.buffer.clear();
        self.readable.notify_all();
        self.writable.notify_all();
    }

    fn flush(&self) {
        let mut state = self.state.lock().unwrap();
        state.buffer.clear();
        self.writable.notify_all();
    }

    fn push_bytes(&self, mut bytes: &[u8]) -> SinkResult<()> {
        while !bytes.is_empty() {
            let mut state = self.state.lock().unwrap();
            while !state.closed && state.buffer.len() >= AUDIO_CAPACITY_BYTES {
                state = self.writable.wait(state).unwrap();
            }
            if state.closed {
                return Err(SinkError::NotConnected(
                    "bridge audio pipe closed".to_string(),
                ));
            }

            let writable = AUDIO_CAPACITY_BYTES - state.buffer.len();
            let chunk = cmp::min(writable, bytes.len());
            state.buffer.extend(bytes[..chunk].iter().copied());
            bytes = &bytes[chunk..];
            self.readable.notify_one();
        }
        Ok(())
    }

    fn read(&self, out: &mut [u8]) -> usize {
        let mut state = self.state.lock().unwrap();
        let count = cmp::min(out.len(), state.buffer.len());
        for byte in out.iter_mut().take(count) {
            *byte = state.buffer.pop_front().unwrap();
        }
        if count > 0 {
            self.writable.notify_all();
        }
        count
    }

    fn buffered_bytes(&self) -> usize {
        self.state.lock().unwrap().buffer.len()
    }
}

struct SpotiampSink {
    pipe: Arc<AudioPipe>,
    started: bool,
}

impl SpotiampSink {
    fn new(pipe: Arc<AudioPipe>) -> Self {
        Self {
            pipe,
            started: false,
        }
    }

    fn write_s16(&self, samples: &[i16]) -> SinkResult<()> {
        let mut bytes = Vec::with_capacity(samples.len() * 2);
        for sample in samples {
            bytes.extend_from_slice(&sample.to_ne_bytes());
        }
        self.pipe.push_bytes(&bytes)
    }
}

impl Sink for SpotiampSink {
    fn start(&mut self) -> SinkResult<()> {
        self.pipe.flush();
        self.started = true;
        Ok(())
    }

    fn stop(&mut self) -> SinkResult<()> {
        self.pipe.flush();
        self.started = false;
        Ok(())
    }

    fn write(&mut self, packet: AudioPacket, converter: &mut Converter) -> SinkResult<()> {
        if !self.started {
            return Ok(());
        }
        match packet {
            AudioPacket::Samples(samples) => {
                let samples = converter.f64_to_s16(&samples);
                self.write_s16(&samples)
            }
            AudioPacket::Raw(bytes) => self.pipe.push_bytes(&bytes),
        }
    }
}

struct Bridge {
    pipe: Arc<AudioPipe>,
    playback_state: Arc<PlaybackState>,
    runtime: Runtime,
    spirc: Spirc,
    task: JoinHandle<()>,
    event_task: JoinHandle<()>,
    running: Arc<AtomicBool>,
    _mixer: Arc<dyn Mixer>,
    _player: Arc<Player>,
    _session: Session,
}

impl Bridge {
    fn shutdown(self) {
        let _ = self.spirc.shutdown();
        self.pipe.close();
        self.running.store(false, Ordering::Release);
        self.event_task.abort();
        self.task.abort();
        self.runtime.shutdown_timeout(Duration::from_secs(2));
    }
}

async fn monitor_player_events(
    mut events: tokio::sync::mpsc::UnboundedReceiver<PlayerEvent>,
    playback_state: Arc<PlaybackState>,
) {
    while let Some(event) = events.recv().await {
        match event {
            PlayerEvent::Loading {
                play_request_id,
                position_ms,
                ..
            } => {
                playback_state.update_event(BridgePlayState::Loading, play_request_id, position_ms);
            }
            PlayerEvent::Playing {
                play_request_id,
                position_ms,
                ..
            } => {
                playback_state.set_play_state(
                    BridgePlayState::Playing,
                    play_request_id,
                    position_ms,
                );
            }
            PlayerEvent::PositionCorrection { .. } | PlayerEvent::PositionChanged { .. } => {}
            PlayerEvent::Seeked {
                play_request_id,
                position_ms,
                ..
            } => {
                playback_state.update_event(BridgePlayState::Loading, play_request_id, position_ms);
            }
            PlayerEvent::Paused {
                play_request_id,
                position_ms,
                ..
            } => {
                playback_state.set_play_state(
                    BridgePlayState::Paused,
                    play_request_id,
                    position_ms,
                );
            }
            PlayerEvent::Stopped {
                play_request_id, ..
            }
            | PlayerEvent::EndOfTrack {
                play_request_id, ..
            }
            | PlayerEvent::Unavailable {
                play_request_id, ..
            } => {
                playback_state.update_event(BridgePlayState::Stopped, play_request_id, 0);
            }
            PlayerEvent::TrackChanged { .. } => {
                playback_state.update(BridgePlayState::Loading, 0);
            }
            _ => {}
        }
    }
}

fn cstr_to_string(value: *const c_char) -> Option<String> {
    if value.is_null() {
        return None;
    }
    let cstr = unsafe { CStr::from_ptr(value) };
    cstr.to_str().ok().map(str::to_string)
}

fn cstr_array_to_strings(values: *const *const c_char, count: usize) -> Result<Vec<String>, i32> {
    if values.is_null() || count == 0 {
        return Err(-1);
    }

    let mut out = Vec::with_capacity(count);
    let slice = unsafe { std::slice::from_raw_parts(values, count) };
    for value in slice {
        let Some(uri) = cstr_to_string(*value) else {
            return Err(-2);
        };
        if !uri.is_empty() {
            out.push(uri);
        }
    }

    if out.is_empty() {
        return Err(-3);
    }
    Ok(out)
}

fn load_options(
    start_playing: i32,
    position_ms: u32,
    playing_track: Option<PlayingTrack>,
) -> LoadRequestOptions {
    LoadRequestOptions {
        start_playing: start_playing != 0,
        seek_to: position_ms,
        playing_track,
        ..LoadRequestOptions::default()
    }
}

fn start_bridge(access_token: String, cache_dir: Option<String>) -> Result<Bridge, String> {
    let pipe = Arc::new(AudioPipe::new());
    let playback_state = Arc::new(PlaybackState::new());
    let runtime = Runtime::new().map_err(|e| e.to_string())?;
    let running = Arc::new(AtomicBool::new(false));

    let init_pipe = pipe.clone();
    let init_playback_state = playback_state.clone();
    let init_running = running.clone();
    let init = runtime.block_on(async move {
        let session_config = SessionConfig {
            device_id: "spotiamp".to_string(),
            ..SessionConfig::default()
        };
        let cache = cache_dir
            .as_deref()
            .and_then(|dir| Cache::new(Some(dir), Some(dir), Some(dir), None).ok());
        let session = Session::new(session_config, cache);
        let credentials = Credentials::with_access_token(access_token);

        let mixer_builder =
            mixer::find(None).ok_or_else(|| "librespot soft mixer not found".to_string())?;
        let mixer = mixer_builder(MixerConfig {
            volume_ctrl: VolumeCtrl::Fixed,
            ..MixerConfig::default()
        })
        .map_err(|e| e.to_string())?;

        let player = Player::new(
            PlayerConfig {
                position_update_interval: Some(Duration::from_millis(500)),
                ..PlayerConfig::default()
            },
            session.clone(),
            mixer.get_soft_volume(),
            move || Box::new(SpotiampSink::new(init_pipe.clone())),
        );
        let player_events = player.get_player_event_channel();

        let connect_config = ConnectConfig {
            name: "Spotiamp".to_string(),
            initial_volume: u16::MAX,
            disable_volume: true,
            volume_steps: 64,
            ..ConnectConfig::default()
        };

        let (spirc, spirc_task) = Spirc::new(
            connect_config,
            session.clone(),
            credentials,
            player.clone(),
            mixer.clone(),
        )
        .await
        .map_err(|e| e.to_string())?;
        spirc.activate().map_err(|e| e.to_string())?;
        init_running.store(true, Ordering::Release);
        Ok::<_, String>((
            spirc,
            spirc_task,
            player_events,
            init_playback_state,
            session,
            player,
            mixer,
        ))
    })?;

    let (spirc, spirc_task, player_events, event_playback_state, session, player, mixer) = init;
    let event_task = runtime.spawn(monitor_player_events(player_events, event_playback_state));
    let task = runtime.spawn(spirc_task);

    Ok(Bridge {
        pipe,
        playback_state,
        runtime,
        spirc,
        task,
        event_task,
        running,
        _mixer: mixer,
        _player: player,
        _session: session,
    })
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_abi_version() -> u32 {
    2
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_version_string() -> *const c_char {
    BRIDGE_VERSION.as_ptr() as *const c_char
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_start(
    access_token: *const c_char,
    cache_dir: *const c_char,
    error_buf: *mut c_char,
    error_buf_size: i32,
) -> i32 {
    let Some(token) = cstr_to_string(access_token) else {
        return -1;
    };
    if token.is_empty() {
        return -2;
    }
    let cache_dir = cstr_to_string(cache_dir);

    let mut bridge = GLOBAL_BRIDGE.lock().unwrap();
    if let Some(existing) = bridge.take() {
        existing.shutdown();
    }

    match start_bridge(token, cache_dir) {
        Ok(new_bridge) => {
            *bridge = Some(new_bridge);
            if !error_buf.is_null() && error_buf_size > 0 {
                unsafe {
                    *error_buf = 0;
                }
            }
            0
        }
        Err(err) => {
            eprintln!("[spotiamp_playback_bridge] start failed: {err}");
            if !error_buf.is_null() && error_buf_size > 0 {
                let err_cstr = std::ffi::CString::new(err).unwrap_or_default();
                let bytes = err_cstr.as_bytes_with_nul();
                let len = std::cmp::min(bytes.len(), error_buf_size as usize - 1);
                unsafe {
                    std::ptr::copy_nonoverlapping(bytes.as_ptr(), error_buf as *mut u8, len);
                    *error_buf.add(len) = 0;
                }
            }
            -3
        }
    }
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_stop() {
    let mut bridge = GLOBAL_BRIDGE.lock().unwrap();
    if let Some(existing) = bridge.take() {
        existing.shutdown();
    }
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_is_running() -> i32 {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    bridge
        .as_ref()
        .map(|bridge| bridge.running.load(Ordering::Acquire) as i32)
        .unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_read(buffer: *mut c_void, buffer_size: usize) -> isize {
    if buffer.is_null() || buffer_size == 0 {
        return 0;
    }

    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -1;
    };

    let out = unsafe { std::slice::from_raw_parts_mut(buffer.cast::<u8>(), buffer_size) };
    let bytes_read = bridge.pipe.read(out);
    bridge.playback_state.advance_audio_bytes(bytes_read);
    bytes_read as isize
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_buffered_bytes() -> usize {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    bridge
        .as_ref()
        .map(|bridge| bridge.pipe.buffered_bytes())
        .unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_snapshot(snapshot: *mut SpPlaybackSnapshot) -> i32 {
    if snapshot.is_null() {
        return -1;
    }

    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -2;
    };

    let value = bridge.playback_state.snapshot(bridge.pipe.buffered_bytes());
    unsafe {
        *snapshot = value;
    }
    0
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_play_state() -> i32 {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    bridge
        .as_ref()
        .map(|bridge| bridge.playback_state.play_state() as i32)
        .unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_position_ms() -> u32 {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    bridge
        .as_ref()
        .map(|bridge| bridge.playback_state.position_ms())
        .unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_flush() {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    if let Some(bridge) = bridge.as_ref() {
        bridge.pipe.flush();
    }
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_load_tracks(
    uris: *const *const c_char,
    count: usize,
    index: u32,
    position_ms: u32,
    start_playing: i32,
) -> i32 {
    let tracks = match cstr_array_to_strings(uris, count) {
        Ok(tracks) => tracks,
        Err(err) => return err,
    };
    if index as usize >= tracks.len() {
        return -4;
    }

    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -5;
    };

    bridge.pipe.flush();
    bridge
        .playback_state
        .begin_command(BridgePlayState::Loading, position_ms);
    let request = LoadRequest::from_tracks(
        tracks,
        load_options(start_playing, position_ms, Some(PlayingTrack::Index(index))),
    );
    bridge.spirc.load(request).map(|_| 0).unwrap_or(-6)
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_load_context(
    context_uri: *const c_char,
    track_uri: *const c_char,
    index: i32,
    position_ms: u32,
    start_playing: i32,
) -> i32 {
    let Some(context) = cstr_to_string(context_uri) else {
        return -1;
    };
    if context.is_empty() {
        return -2;
    }

    let track = cstr_to_string(track_uri).filter(|value| !value.is_empty());
    let playing_track = if let Some(track) = track {
        Some(PlayingTrack::Uri(track))
    } else if index >= 0 {
        Some(PlayingTrack::Index(index as u32))
    } else {
        None
    };

    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -3;
    };

    bridge.pipe.flush();
    bridge
        .playback_state
        .begin_command(BridgePlayState::Loading, position_ms);
    let request = LoadRequest::from_context_uri(
        context,
        load_options(start_playing, position_ms, playing_track),
    );
    bridge.spirc.load(request).map(|_| 0).unwrap_or(-4)
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_resolve_context_tracks(
    context_uri: *const c_char,
    buffer: *mut c_char,
    buffer_size: usize,
) -> i32 {
    let Some(context_uri) = cstr_to_string(context_uri) else {
        return -1;
    };
    if buffer.is_null() || buffer_size == 0 {
        return -2;
    }

    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let result = bridge.as_ref().ok_or(()).and_then(|bridge| {
        bridge
            .runtime
            .block_on(bridge._session.spclient().get_context(&context_uri))
            .map_err(|_| ())
    });
    let Ok(context) = result else {
        return -3;
    };

    let mut output = String::new();
    for page in context.pages {
        for track in page.tracks {
            let uri = track.uri();
            if uri.starts_with("spotify:track:") {
                output.push_str(uri);
                output.push('\n');
            }
        }
    }
    if output.len() + 1 > buffer_size {
        return -4;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(output.as_ptr(), buffer.cast::<u8>(), output.len());
        *buffer.add(output.len()) = 0;
    }
    output.len() as i32
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_resolve_discover_weekly_playlist(
    buffer: *mut c_char,
    buffer_size: usize,
) -> i32 {
    if buffer.is_null() || buffer_size == 0 {
        return -1;
    }

    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -2;
    };

    let mut found: Option<String> = None;
    let mut last_error = false;
    for from in (0..600usize).step_by(120) {
        let result = bridge
            .runtime
            .block_on(bridge._session.spclient().get_rootlist(from, Some(120)));
        let Ok(bytes) = result else {
            eprintln!(
                "[spotiamp_playback_bridge] Discover Weekly rootlist request failed at offset {from}"
            );
            last_error = true;
            break;
        };
        let Ok(rootlist) = SelectedListContent::parse_from_bytes(&bytes) else {
            eprintln!(
                "[spotiamp_playback_bridge] Discover Weekly rootlist parse failed at offset {from}"
            );
            last_error = true;
            break;
        };

        let contents = rootlist.contents.get_or_default();
        eprintln!(
            "[spotiamp_playback_bridge] rootlist page offset={from} items={} meta_items={} total_length={}",
            contents.items.len(),
            contents.meta_items.len(),
            rootlist.length()
        );

        if let Some(uri) = discover_weekly_from_rootlist(&rootlist) {
            found = Some(uri);
            break;
        }

        if contents.items.len() < 120 {
            break;
        }
    }

    let Some(output) = found else {
        return if last_error { -3 } else { 0 };
    };

    if output.len() + 1 > buffer_size {
        return -4;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(output.as_ptr(), buffer.cast::<u8>(), output.len());
        *buffer.add(output.len()) = 0;
    }
    output.len() as i32
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_resolve_playlist_tracks(
    playlist_uri: *const c_char,
    buffer: *mut c_char,
    buffer_size: usize,
) -> i32 {
    let Some(playlist_uri) = cstr_to_string(playlist_uri) else {
        return -1;
    };
    if buffer.is_null() || buffer_size == 0 {
        return -2;
    }

    let Ok(spotify_uri) = SpotifyUri::from_uri(&playlist_uri) else {
        return -3;
    };
    let SpotifyUri::Playlist { id, .. } = spotify_uri else {
        return -4;
    };

    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -5;
    };

    let result = bridge
        .runtime
        .block_on(bridge._session.spclient().get_playlist(&id));
    let Ok(bytes) = result else {
        eprintln!(
            "[spotiamp_playback_bridge] playlist/v2 request failed for {playlist_uri}"
        );
        return -6;
    };
    let Ok(playlist) = SelectedListContent::parse_from_bytes(&bytes) else {
        eprintln!(
            "[spotiamp_playback_bridge] playlist/v2 parse failed for {playlist_uri}"
        );
        return -7;
    };

    let contents = playlist.contents.get_or_default();
    let mut output = String::new();
    for item in &contents.items {
        let uri = item.uri();
        if uri.starts_with("spotify:track:") {
            output.push_str(uri);
            output.push('\n');
        }
    }

    eprintln!(
        "[spotiamp_playback_bridge] playlist/v2 {playlist_uri} returned {} track uris",
        output.lines().count()
    );

    if output.is_empty() {
        return 0;
    }
    if output.len() + 1 > buffer_size {
        return -8;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(output.as_ptr(), buffer.cast::<u8>(), output.len());
        *buffer.add(output.len()) = 0;
    }
    output.len() as i32
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_play() -> i32 {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -1;
    };
    bridge._player.play();
    bridge
        .playback_state
        .force_play_state(BridgePlayState::Playing);
    0
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_pause() -> i32 {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -1;
    };
    bridge._player.pause();
    bridge
        .playback_state
        .force_play_state(BridgePlayState::Paused);
    bridge.pipe.flush();
    0
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_stop_playback() -> i32 {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -1;
    };
    bridge.pipe.flush();
    bridge._player.stop();
    bridge
        .playback_state
        .begin_command(BridgePlayState::Stopped, 0);
    0
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_seek(position_ms: u32) -> i32 {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -1;
    };
    bridge.pipe.flush();
    bridge
        .playback_state
        .begin_command(BridgePlayState::Loading, position_ms);
    bridge._player.seek(position_ms);
    0
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_next() -> i32 {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -1;
    };
    bridge.pipe.flush();
    bridge
        .playback_state
        .begin_command(BridgePlayState::Loading, 0);
    bridge.spirc.next().map(|_| 0).unwrap_or(-2)
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_prev() -> i32 {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -1;
    };
    bridge.pipe.flush();
    bridge
        .playback_state
        .begin_command(BridgePlayState::Loading, 0);
    bridge.spirc.prev().map(|_| 0).unwrap_or(-2)
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_set_shuffle(enabled: i32) -> i32 {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -1;
    };
    bridge.spirc.shuffle(enabled != 0).map(|_| 0).unwrap_or(-2)
}

#[no_mangle]
pub extern "C" fn sp_playback_bridge_set_repeat(mode: i32) -> i32 {
    let bridge = GLOBAL_BRIDGE.lock().unwrap();
    let Some(bridge) = bridge.as_ref() else {
        return -1;
    };
    if mode == 2 {
        bridge.spirc.repeat_track(true).map(|_| 0).unwrap_or(-2)
    } else {
        if let Err(_) = bridge.spirc.repeat_track(false) {
            return -2;
        }
        bridge.spirc.repeat(mode != 0).map(|_| 0).unwrap_or(-3)
    }
}
