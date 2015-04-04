require 'dxruby'
require_relative "soundbuffer"

#
# Wav File Read
#
filename = Window.open_filename([["WAV File(*.wav)", "*.wav"]], "WAV File")
exit if filename.nil?

channels, samples_per_sec, bits_per_sample = 0, 0, 0
wave = File.open(filename, binmode: true) { |f|
  e_not_wav   = "this file may not be in the WAV format"
  e_unsupport = "unsupported WAV file"
  e_broken    = "broken WAV file"
  header_size = 44
  head = f.read(header_size)
  data = head.unpack("a4Va4a4VvvVVvva4V")
  raise e_not_wav   if data[0] != "RIFF"
  _body_size        =  data[1]
  raise e_not_wav   if data[2] != "WAVE"
  raise e_not_wav   if data[3] != "fmt "
  raise e_unsupport if data[4] != 16     # fmt chunk size
  raise e_unsupport if data[5] != 1      # liner PCM fromat
  channels          =  data[6]
  raise e_unsupport unless channels == 1 || channels == 2
  samples_per_sec   =  data[7]
  _bytes_per_sec    =  data[8]
  bytes_per_sample  =  data[9]
  bits_per_sample   =  data[10]
  raise e_unsupport if bits_per_sample != 16
  raise e_broken    if bytes_per_sample != bits_per_sample / 8 * channels
  raise e_broken    if data[11] != "data"
  data_size         =  data[12]
  raise e_broken    unless (data_size % bytes_per_sample).zero?
  #raise e_unsupport unless 4 + _body_size == header_size + data_size
  total = data_size / bytes_per_sample
  f.read(data_size)
}
#
# SoundBuffer object create
#
sound = SoundBuffer.new(wave, channels, samples_per_sec, bits_per_sample, effect: true).tap { |s|
  s.loop_start = 0
  s.loop_end   = 0
  s.loop_count = 0
}
puts "Create Reverce wave data"
r_sound = SoundBuffer.new(
  sound.size,
  sound.channels,
  sound.samples_per_sec,
  sound.bits_per_sample,
  effect: sound.effectable?
).tap { |s|
  r_wave = (wave.size / 4).downto(0).inject(""){ |w, i| w << wave[i * 4, 4] }
  s.write(r_wave)
}

fx = [SoundBuffer::FXWavesReverb.new]
font = Font.default

play_sound = sound
pcm_pos = 0
volume  = -1000
pitch   = 0.0
speed   = 1
played  = false
frequency       = play_sound.frequency
mouse_seek_step = samples_per_sec / 100
total_time      = play_sound.total / samples_per_sec
#
# Main Loop
#
Window.loop do
  #
  # Play Control
  #
  if Input.key_push?(K_SPACE) && speed == 1
    played ? play_sound.pause : play_sound.play ;
    played = play_sound.playing?
  end

  if Input.key_push?(K_S)
    play_sound.stop
  end
  #
  # Rewind & Fast Forward
  #
  unless Input.key_down?(K_LEFT)
    if Input.key_push?(K_RIGHT)
      speed = 4
      played = play_sound.playing?
      play_sound.play unless played
    end
    if Input.key_release?(K_RIGHT)
      speed = 1
      played ? play_sound.play : play_sound.pause ;
    end
  end
  unless Input.key_down?(K_RIGHT)
    if Input.key_push?(K_LEFT)
      speed = -4
      played = play_sound.playing?
      play_sound.pause
      pos = play_sound.pcm_pos
      play_sound = r_sound
      play_sound.pcm_pos = play_sound.total - pos
      play_sound.play #unless played
    end
    if Input.key_release?(K_LEFT)
      speed = 1
      play_sound.pause
      pos = play_sound.pcm_pos
      play_sound = sound
      play_sound.pcm_pos = play_sound.total - pos
      played ? play_sound.play : play_sound.pause ;
    end
  end
  #
  # Mouse wheel Seeking
  #
  unless Input.mouse_wheel_pos.zero?
    pcm_pos = play_sound.pcm_pos
    pcm_pos += Input.mouse_wheel_pos * mouse_seek_step
    pcm_pos = 0                    if pcm_pos < 0
    pcm_pos = play_sound.total - 1 if pcm_pos > play_sound.total
    Input.mouse_wheel_pos = 0
    play_sound.pcm_pos = pcm_pos
  end
  #
  # A-B Repeat Control
  #
  if Input.key_push?(K_UP) && speed == 1
    if sound.loop_start.zero?
      sound.loop_start = sound.pcm_pos
    elsif sound.loop_end.zero?
      sound.stop_and_play {
        sound.loop_end = sound.pcm_pos
      }
      sound.loop       = true
      sound.pcm_pos    = sound.loop_start
    else
      sound.loop       = false
      sound.loop_start = 0
      sound.stop_and_play {
        sound.loop_end = 0
      }
    end
  end
  #
  # Volume Control
  #
  volume += 10  if Input.key_down?(K_A)
  volume -= 10  if Input.key_down?(K_Z)
  #
  # Pitch Control
  #
  pitch  += 1   if Input.key_push?(K_D)
  pitch  -= 1   if Input.key_push?(K_C)
  #
  # FX Control
  #
  if Input.key_down?(K_X)
    play_sound.effect = fx if     play_sound.effectable? && play_sound.effect.empty?
  else
    play_sound.effect = [] unless play_sound.effectable? && play_sound.effect.empty?
  end
  #
  # Refrect Parameters
  #
  volume    = SoundBuffer::DSBVOLUME_MAX if volume > SoundBuffer::DSBVOLUME_MAX
  volume    = SoundBuffer::DSBVOLUME_MIN if volume < SoundBuffer::DSBVOLUME_MIN
  play_sound.volume = volume
  frequency = samples_per_sec * speed.abs * (2.0 ** (pitch / 12.0))
  frequency = SoundBuffer::DSBFREQUENCY_MAX if frequency > SoundBuffer::DSBFREQUENCY_MAX
  frequency = SoundBuffer::DSBFREQUENCY_MIN if frequency < SoundBuffer::DSBFREQUENCY_MIN
  play_sound.frequency = frequency
  #
  # View
  #
  Window.draw_font(  0,   0, "FPS:#{Window.fps} Total:#{play_sound.total} Position:#{play_sound.pcm_pos}", font)
  Window.draw_font(  0,  25, "TotalTime:#{format('%.3f', total_time)}sec Time:#{format('%.3f', play_sound.pcm_pos.to_f / samples_per_sec)}sec",  Font.default)
  Window.draw_font(  0,  50, "Pitch: #{pitch >= 0 ? '+' : '';}#{pitch} Freq:#{play_sound.frequency}Hz",  font)
  Window.draw_font(  0,  75, "Volume: #{play_sound.volume}",  font)
  Window.draw_line(  0, 120, 639, 120, C_WHITE)
  cur_x = Window.width * (speed > 0 ? play_sound.pcm_pos : play_sound.total - play_sound.pcm_pos) / play_sound.total - font.size / 2
  Window.draw_font(cur_x, 120, '↑',  font, color:C_GREEN)
  cur_A = Window.width * sound.loop_start / play_sound.total - font.size / 2
  Window.draw_font(cur_A, 120, '↑',  font, color:C_YELLOW) unless sound.loop_start.zero?
  cur_B = Window.width * sound.loop_end   / play_sound.total - font.size / 2
  Window.draw_font(cur_B, 120, '↑',  font, color:C_RED)    unless sound.loop_end.zero?
  Window.draw_font(272, 220, play_sound.playing? ? "PLAY" : "STOP",         font)
  Window.draw_font(  0, 390, "[A]Volume↑ [S]Stop [D]Pitch↑ [↑]RepeatA/B/Reset", font)
  Window.draw_font(  0, 420, "[Z]Volume↓ [X]FX On [C]Pitch↓ [←]REW [→]FF", font)
  Window.draw_font(  0, 450, "[SPACE]Play or Pause", font)
end
