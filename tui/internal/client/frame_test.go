package client

import (
	"encoding/json"
	"testing"
)

func decode(s string) Frame {
	var f Frame
	_ = json.Unmarshal([]byte(s), &f)
	return f
}

// 推送流用 event 作键，回放用 type：解出来是同一帧（ADR-0020）
func TestPushAndReplayDecodeAlike(t *testing.T) {
	push := decode(`{"event":"tool_output","data":{"root":"r","call_id":"c1","text":"a\n"}}`)
	replay := decode(`{"type":"tool_output","data":{"root":"r","call_id":"c1","text":"a\n"}}`)
	want := Frame{Event: "tool_output", Root: "r", CallID: "c1", Text: "a\n"}
	if push.Event != want.Event || push.Root != want.Root || push.CallID != want.CallID || push.Text != want.Text {
		t.Errorf("推送帧 = %+v, want %+v", push, want)
	}
	if replay.Event != push.Event || replay.CallID != push.CallID || replay.Text != push.Text {
		t.Errorf("回放帧 = %+v, 应与推送帧 %+v 相同", replay, push)
	}
}

// 载荷是开放键集：不认识的键、类型对不上的键都不连累别的字段
func TestOpenKeySet(t *testing.T) {
	f := decode(`{"event":"permission_request","data":{"id":"a1","tool":"bash","params":{"cmd":"ls"},"cost":"x","extra":1}}`)
	if f.Event != "permission_request" || f.ID != "a1" || f.Tool != "bash" || string(f.Params) != `{"cmd":"ls"}` {
		t.Errorf("帧 = %+v", f)
	}
}
