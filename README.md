# SoundBuffer
Play sound using iDirectSoundBuffer8 from Ruby

## ライセンス
zlib/libpng License

## 求む
* DSERR_BUFFERLOSTがソフトウェアーバッファーでも起きるのかどうか？　の情報
* SetNotificationPositionsの使い方。これがわからないと区間ループが実装できない。
* C拡張からオプション引数の使い方。

## 使い方
* soundbuffer.so: これ単独でSoundBufferクラスを提供する。
* soundbuffer.rb: エフェクトを使うときに便利なStructや追加メソッドを提供する。

## 実装インスタンス・メソッド
play, repeat, pause, stop, playing?, repeating?, pausing?<br />
size, write, write_sync, stop_and_play<br />
get_volume, set_volume, get_pan, set_pan, get_frequency, set_frequency<br />
volume, volume=, pan, pna=, frequency, frequency=<br />
etc...

## 今後の予定
* プライマリー･バッファーへのアクセスメソッド（マスターボリュームのため）
* Notifyの利用、それに伴い区間ループの実装
* Stringのようなメソッド群（ただし、String互換ではない）
* デュプリケートの実装。同一音源の同時再生に必要になる。
* 例外を適切なものにする（たとえばArgumentErrorを使用する）
* サンプルコード
* テストコード

## 現在取り組んでいること
* play_pos, play_pos= をpos, pos= に改名するかどうか。<< メソッドを追加するかに左右される。
* to_sメソッドを用意したが、本来iDirectSoundBuffer8からの読み取りは行ってはいけない。
* プライマリーバッファーへのアクセス。マスターボリュームの実装に必要。
* Duplicateの実装。同時発音に必要。ただしFXとの併用はできない。
* BEEP

## 実装しないもの
* ハードウェアーバッファーの利用。
* ３D音響機能の実装。これは、まったく別のクラスとして設計する必要がある。
* 同時発音のマネージメント。Ruby側のコードで書く。あとでモジュールかクラスを作成して添付するかもしれない。
* 音量エンベロープ。これはRubyコードで可能だし、そうすべきと考えている。
