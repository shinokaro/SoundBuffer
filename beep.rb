require_relative 'soundbuffer.so'

module Beep
  extend self

  @@beep = SoundBuffer.new(("\xff" * 24 + "\x00" * 24) * 200, 1, 48000, 8)

  def beep(sec = 1, hz = 1000, vol = 0)
    @@beep.stop_and_play { beep_on(hz, vol) }
    sleep sec
    beep_off
    self
  end

  def beep!(*args)
    Thread.new do
      begin
        beep(*args)
      ensure
        beep_off
      end
    end
    self
  end

  def beep_on(hz = 1000, vol = 0)
    @@beep.set_frequency(hz * 48)
    @@beep.set_volume(vol)
    @@beep.repeat
    self
  end

  def beep_off
    @@beep.stop
    self
  end

  def beeping?
    @@beep.playing?
  end

  def beeps(*args)
    args.each_slice(3) { |(sec, hz, vol)|
      beep_on(hz, vol)
      sleep sec
    }
    beep_off
    self
  end

  def beeps!(*args)
    Thread.new do
      begin
        beeps(*args)
      ensure
        beep_off
      end
    end
    self
  end
end
