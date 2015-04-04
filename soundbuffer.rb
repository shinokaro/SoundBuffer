require_relative 'soundbuffer.so'

class SoundBuffer
  FXList = [
    [FX_GARGLE,       :FXGargle,      %w(RateHz WaveShape)],
    [FX_CHORUS,       :FXChorus,      %w(WetDryMix Depth Feedback Frequency Waveform Delay Phase)],
    [FX_FLANGER,      :FXFlanger,     %w(WetDryMix Depth Feedback Frequency Waveform Delay Phase)],
    [FX_ECHO,         :FXEcho,        %w(WetDryMix Feedback LeftDelay RightDelay PanDelay)],
    [FX_DISTORTION,   :FXDistortion,  %w(Gain Edge PostEQCenterFrequency PostEQBandwidth PreLowpassCutoff)],
    [FX_COMPRESSOR,   :FXCompressor,  %w(Gain Attack Release Threshold Ratio Predelay)],
    [FX_PARAM_EQ,     :FXParamEq,     %w(Center Bandwidth Gain)],
    [FX_I3DL2_REVERB, :FXI3DL2Reverb, %w(Room RoomHF RoomRolloffFactor DecayTime DecayHFRatio Reflections
                                        ReflectionsDelay Reverb ReverbDelay Diffusion Density HFReference)],
    [FX_WAVES_REVERB, :FXWavesReverb, %w(InGain ReverbMix ReverbTime HighFreqRTRatio)]
  ].map { |(const, name, accessors)|
    const_set(name, Struct.new(*accessors.map!(&:to_sym)) {
      eval "
        def self.to_i
          #{const}
        end

        def initialize(*args)
          args.empty? ? super(*#{name}_Default) : super ;
        end

        def to_i
          #{const}
        end"
    })
  }.freeze

  FXGargle_Default      = [20, DSFXGARGLE_WAVE_TRIANGLE].freeze
  FXChorus_Default      = [50.0, 10.0, 25.0, 1.1, DSFXCHORUS_WAVE_SIN, 16.0, DSFXCHORUS_PHASE_90].freeze
  FXFlanger_Default     = [50.0, 100.0, -50.0, 0.25, DSFXFLANGER_WAVE_SIN, 0.2, DSFXFLANGER_PHASE_ZERO].freeze
  FXEcho_Default        = [50.0, 50.0, 500.0, 500.0, 0].freeze
  FXDistortion_Default  = [-18.0, 15.0, 2400.0, 2400.0, 8000.0].freeze
  FXCompressor_Default  = [0.0, 10.0, 200.0, -20.0, 3.0, 4.0].freeze
  FXParamEq_Default     = [8000.0, 12.0, 0.0].freeze
  FXI3DL2Reverb_Default = [-1000, -100, 0.0, 1.49, 0.83, -2602, 0.007, 200, 0.011, 100.0, 100.0, 5000.0].freeze
  FXWavesReverb_Default = [0.0, 0.0, 1000.0, 0.001].freeze

  def effect
    get_effect.each_with_index.map { |fx_num, idx| FXList.find { |fx| fx.to_i == fx_num }.new(*get_effect_param(idx)) }
  end

  def effect=(fx_lst)
    stop_and_play{
      set_effect(*fx_lst.map(&:to_i))
      fx_lst.each_with_index { |fx, idx| set_effect_param(idx, *fx.to_a) }
    }
  end

  def jump(nth)
    self.pcm_pos = get_notify[nth]
  end

  def loop=(flag)
    @__thread = nil unless instance_variable_defined?(:@__thread)
    if flag
      set_loop(true)
      if @__thread.nil? || !@__thread.alive?
        @__thread = Thread.new { loop { wait; p :stop } while true }
      end
    else
      set_loop(false)
      if !@__thread.nil? || @__thread.alive?
        @__thread.kill
        @__thread = nil
      end
    end
  end
end
