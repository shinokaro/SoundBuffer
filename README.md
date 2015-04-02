# SoundBuffer
Play sound using iDirectSoundBuffer8 from Ruby

## ライセンス
zlib/libpng License

## 求む
* DSERR_BUFFERLOSTがソフトウェアーバッファーでも起きるのかどうか？　の情報

## 使い方
* soundbuffer.so: これ単独でSoundBufferクラスを提供する。
* soundbuffer.rb: エフェクトを使うときに便利なStructや追加メソッドを提供する。
* beep.rb: Beepモジュール。Beep音を手軽に鳴らすためのモジュール。

上記ファイルのどれかをrequireする。
### Beepモジュールの例
```ruby
require "beep" # エラーが出る場合はパスを通しておくか、相対、絶対パスで指定する
Beep.beep      # モジュールメソッドを使ったBeep音。1秒間、1kHz、最大音量でなる。
include Beep   # includeしてモジュール名を省略する。
sleep 0.1
beep(1, 1000, 0) # 引数は秒数、周波数、音量。音量は０が最大。小さくしたければマイナスの数値を入れる。
sleep 0.1
beep!(4, 1000, 0) # beep!はスレッドでBeepを鳴らすのでブロックしない。
begin
  sleep 0.1        # beep音が鳴り止むのを待つループ。少し待たないと鳴り始めない。
end while beeping? # beeping? はBeep音が鳴っていればtrue。
beeps(0.2, 1000, 0,
      0.2, 500, -100) # sec, hz, volの順に鳴らしたい音の数だけ指定する。曲を奏でる。
sleep 1
beep_on(1000, 0) # Beep音を鳴らしっぱなしにする。引数は周波数、音量の２つ。
sleep 0.5
beep_off         # Beep音を止める。Beep音は1つしか同時に鳴らないので、これで強制的に音を止めることもできる。
sleep 0.5
Thread.new {
  hz = 100
  while hz < 1000
    beep_on(hz)
    sleep 0.01
    hz += 2
  end
  beep_off
}.join # スレッドからのBeep音。ここではjoinしてスレッドの終了を待っている。
```

## 実装インスタンス・メソッド
play, repeat, pause, stop, playing?, repeating?, pausing?<br />
size, write, write_sync, stop_and_play<br />
get_volume, set_volume, get_pan, set_pan, get_frequency, set_frequency<br />
volume, volume=, pan, pan=, frequency, frequency=<br />
etc...

## 今後の予定
* 例外を適切なものにする（たとえばArgumentErrorを使用する）
* サンプルコード
* テストコード

## 現在取り組んでいること
* to_sメソッド。
* waitスレッドを走らせないと区間ループが作動しない。背後で走らせる仕組み。

## 実装しないもの
* readメソッド。DirectSoundの仕様によりバッファーの読み取りは禁止（アドレス不定）されている。ただし、代替としてto_sメソッドは準備中。
* ハードウェアーバッファーの利用。DirectX9以降はDirectSoundはWinOSによるソフトウェアー処理になっているため。
* ３D音響機能の実装。これは、まったく別のクラスとして設計する必要がある。
* 同時発音のマネージメント。Ruby側のコードで書く。あとでモジュールかクラスを作成して添付するかもしれない。
* 音量エンベロープ。これはRubyコードで可能だし、そうすべきと考えている。
