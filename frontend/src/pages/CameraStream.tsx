import { useState, useEffect, useRef } from "react";
import { useNavigate } from "react-router";
import { io, type Socket } from "socket.io-client";
import { Button } from "../components/button";
import { Card, CardContent, CardHeader, CardTitle } from "../components/card";
import { Badge } from "../components/badge";
import { Switch } from "../components/switch";
import { Label } from "../components/label";
import {
  Camera,
  PawPrint,
  LogOut,
  Video,
  VideoOff,
  Maximize2,
  Minimize2,
  Bell,
  Settings,
  ArrowUp,
  ArrowDown,
  ArrowLeft,
  ArrowRight,
} from "lucide-react";
import { toast } from "sonner";

type CurrentUser = {
  name: string;
  username: string;
};

type MotionSignal = {
  detected?: boolean;
  detected_at?: string | null;
  source?: string | null;
};

type CameraFramePayload = {
  image?: string;
  captured_at?: string;
};

export function CameraStream() {
  const navigate = useNavigate();
  const [currentUser, setCurrentUser] = useState<CurrentUser | null>(null);
  const [isStreaming, setIsStreaming] = useState(true);
  const [motionDetection, setMotionDetection] = useState(true);
  const [isFullscreen, setIsFullscreen] = useState(false);
  const [isTriggeringBuzzer, setIsTriggeringBuzzer] = useState(false);
  const [isSocketConnected, setIsSocketConnected] = useState(false);
  const [latestFrame, setLatestFrame] = useState<string | null>(null);
  const [latestFrameAt, setLatestFrameAt] = useState<string | null>(null);
  const [activity, setActivity] = useState<string[]>([]);
  const [, setLastMotionSignalAt] = useState<string | null>(null);
  const streamContainerRef = useRef<HTMLDivElement | null>(null);
  const socketRef = useRef<Socket | null>(null);
  const streamSubscribedRef = useRef(false);
  const motionDetectionRef = useRef(motionDetection);

  useEffect(() => {
    motionDetectionRef.current = motionDetection;
  }, [motionDetection]);

  useEffect(() => {
    let isMounted = true;

    const loadCurrentUser = async () => {
      try {
        const response = await fetch("/api/me", {
          credentials: "include",
        });

        if (!response.ok) {
          throw new Error("Not authenticated");
        }

        const data = (await response.json()) as { user?: CurrentUser };
        if (!data.user) {
          throw new Error("Not authenticated");
        }

        if (isMounted) {
          setCurrentUser(data.user);
          localStorage.setItem("petcam_current_user", JSON.stringify(data.user));
        }
      } catch {
        localStorage.removeItem("petcam_current_user");
        toast.error("Please login first!");
        navigate("/");
      }
    };

    loadCurrentUser();

    return () => {
      isMounted = false;
    };
  }, [navigate]);

  useEffect(() => {
    const socket = io({
      withCredentials: true,
      path: "/socket.io",
      transports: ["polling"],
      upgrade: false,
    });
    socketRef.current = socket;

    const onConnect = () => {
      setIsSocketConnected(true);
    };

    const onDisconnect = () => {
      setIsSocketConnected(false);
      streamSubscribedRef.current = false;
    };

    const onMotionSignal = (data: MotionSignal) => {
      if (!motionDetectionRef.current || !data.detected || !data.detected_at) {
        return;
      }

      setLastMotionSignalAt((prev) => {
        if (prev === data.detected_at) {
          return prev;
        }

        setActivity((current) => {
          const newActivity = [
            ...current,
            `${new Date().toLocaleTimeString()} - 🐾 Movement detected (${data.source || "sensor"})`,
          ];
          return newActivity.slice(-5);
        });
        return data.detected_at ?? prev;
      });
    };

    const onCameraFrame = (data: CameraFramePayload) => {
      if (!data.image) {
        return;
      }

      setLatestFrame(data.image);
      setLatestFrameAt(data.captured_at ?? new Date().toISOString());
    };

    const onCameraStreamError = (data: { message?: string }) => {
      toast.error(data.message || "Camera stream error on backend.");
    };

    socket.on("connect", onConnect);
    socket.on("disconnect", onDisconnect);
    socket.on("motion_signal", onMotionSignal);
    socket.on("camera_frame", onCameraFrame);
    socket.on("camera_stream_error", onCameraStreamError);

    return () => {
      if (socket.connected && streamSubscribedRef.current) {
        socket.emit("unsubscribe_camera_stream");
        streamSubscribedRef.current = false;
      }
      socket.off("connect", onConnect);
      socket.off("disconnect", onDisconnect);
      socket.off("motion_signal", onMotionSignal);
      socket.off("camera_frame", onCameraFrame);
      socket.off("camera_stream_error", onCameraStreamError);
      socket.disconnect();
      socketRef.current = null;
      setIsSocketConnected(false);
    };
  }, []);

  useEffect(() => {
    const socket = socketRef.current;
    if (!socket || !socket.connected) {
      return;
    }

    if (isStreaming && !streamSubscribedRef.current) {
      socket.emit(
        "subscribe_camera_stream",
        { interval_ms: 66 },
        (response?: { ok?: boolean; message?: string }) => {
          if (!response?.ok) {
            toast.error(response?.message || "Could not subscribe to camera stream.");
            return;
          }

          streamSubscribedRef.current = true;
        }
      );
      return;
    }

    if (!isStreaming && streamSubscribedRef.current) {
      socket.emit("unsubscribe_camera_stream", (response?: { ok?: boolean; message?: string }) => {
        if (!response?.ok) {
          toast.error(response?.message || "Could not pause backend stream.");
          return;
        }

        streamSubscribedRef.current = false;
      });
    }
  }, [isSocketConnected, isStreaming]);

  const emitSocketEvent = <TResponse,>(event: string, payload?: unknown): Promise<TResponse> => {
    const socket = socketRef.current;
    if (!socket || !socket.connected) {
      return Promise.reject(new Error("Socket not connected"));
    }

    return new Promise<TResponse>((resolve, reject) => {
      socket.timeout(2000).emit(event, payload, (error: unknown, response: TResponse) => {
        if (error) {
          reject(error);
          return;
        }
        resolve(response);
      });
    });
  };

  useEffect(() => {
    const onFullscreenChange = () => {
      setIsFullscreen(document.fullscreenElement === streamContainerRef.current);
    };

    document.addEventListener("fullscreenchange", onFullscreenChange);
    return () => {
      document.removeEventListener("fullscreenchange", onFullscreenChange);
    };
  }, []);

  const handleLogout = async () => {
    try {
      await fetch("/api/logout", {
        method: "POST",
        credentials: "include",
      });
    } catch {
      // Best effort logout on backend.
    } finally {
      localStorage.removeItem("petcam_current_user");
      toast.success("Logged out successfully! 👋");
      navigate("/");
    }
  };

  const toggleStream = () => {
    setIsStreaming(!isStreaming);
    toast.info(isStreaming ? "Stream paused" : "Stream resumed");
  };

  const handleFullscreen = async () => {
    try {
      if (!document.fullscreenElement && streamContainerRef.current) {
        await streamContainerRef.current.requestFullscreen();
      } else if (document.fullscreenElement) {
        await document.exitFullscreen();
      }
    } catch {
      toast.error("Could not toggle fullscreen on this browser.");
    }
  };

  const handleCameraMove = async (direction: "up" | "down" | "left" | "right") => {
    try {
      if (isSocketConnected) {
        const socketResponse = await emitSocketEvent<{ ok?: boolean; message?: string }>("camera_move", {
          direction,
        });
        if (!socketResponse?.ok) {
          toast.error(socketResponse?.message || "Could not send camera command.");
          return;
        }

        toast.info(`Move camera ${direction}`);
        return;
      }

      const response = await fetch("/api/camera/move", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
        },
        credentials: "include",
        body: JSON.stringify({ direction }),
      });

      const data = (await response.json()) as { message?: string };
      if (!response.ok) {
        toast.error(data.message || "Could not send camera command.");
        return;
      }

      toast.info(`Move camera ${direction}`);
    } catch {
      toast.error("Could not reach backend camera controls endpoint.");
    }
  };

  const handleBuzzerTrigger = async () => {
    if (isTriggeringBuzzer) {
      return;
    }

    setIsTriggeringBuzzer(true);
    try {
      if (isSocketConnected) {
        const socketResponse = await emitSocketEvent<{ ok?: boolean; message?: string }>("trigger_buzzer");
        if (!socketResponse?.ok) {
          toast.error(socketResponse?.message || "Could not trigger buzzer.");
          return;
        }

        toast.success("Buzzer signal sent to backend 🔊");
        return;
      }

      const response = await fetch("/api/buzzer", {
        method: "POST",
        credentials: "include",
      });

      const data = (await response.json()) as { message?: string };
      if (!response.ok) {
        toast.error(data.message || "Could not trigger buzzer.");
        return;
      }

      toast.success("Buzzer signal sent to backend 🔊");
    } catch {
      toast.error("Could not reach backend buzzer endpoint.");
    } finally {
      setIsTriggeringBuzzer(false);
    }
  };

  const controlButtonClass = "cursor-pointer border-zinc-900 bg-transparent text-zinc-900 hover:bg-black/5";

  return (
    <div className="min-h-screen bg-gradient-to-br from-pink-50 via-purple-50 to-blue-50">
      {/* Header */}
      <header className="bg-white/80 backdrop-blur-sm border-b-2 border-purple-200 shadow-sm">
        <div className="max-w-7xl mx-auto px-4 py-4 flex items-center justify-between">
          <div className="flex items-center gap-3">
            <div className="size-10 bg-gradient-to-br from-pink-400 to-purple-500 rounded-full flex items-center justify-center">
              <Camera className="size-6 text-white" />
            </div>
            <div>
              <h1 className="font-bold bg-gradient-to-r from-pink-500 to-purple-500 bg-clip-text text-transparent">
                PetCam
              </h1>
              <p className="text-sm text-gray-600">Welcome, {currentUser?.name}! 👋</p>
            </div>
          </div>
          <Button
            onClick={handleLogout}
            variant="outline"
            className="border-pink-300 hover:bg-pink-50"
          >
            <LogOut className="size-4 mr-2" />
            Logout
          </Button>
        </div>
      </header>

      <div className="max-w-7xl mx-auto px-4 py-8">
        <div className="grid grid-cols-1 lg:grid-cols-3 gap-6">
          {/* Main Camera Stream */}
          <div className="lg:col-span-2 space-y-4">
            <Card className="border-2 border-purple-200 shadow-xl overflow-hidden gap-0">
              <CardHeader className="bg-gradient-to-r from-pink-100 to-purple-100 pb-4">
                <div className="flex items-center justify-between">
                  <CardTitle className="flex items-center gap-2">
                    <Video className="size-5 text-pink-500" />
                    Felix
                  </CardTitle>
                  <Badge className="bg-green-500 text-white animate-pulse">
                    <span className="size-2 bg-white rounded-full mr-2 inline-block" />
                    LIVE
                  </Badge>
                </div>
              </CardHeader>
              <CardContent className="p-0 [&:last-child]:pb-0">
                <div ref={streamContainerRef} className={`bg-black ${isFullscreen ? "h-screen flex flex-col" : ""}`}>
                  <div className={`relative bg-gray-900 ${isFullscreen ? "flex-1 min-h-0" : "aspect-video"}`}>
                    {isStreaming ? (
                      <>
                        {latestFrame ? (
                          <img
                            src={latestFrame}
                            alt="Camera stream"
                            className="w-full h-full object-cover scale-x-[-1]"
                          />
                        ) : (
                          <div className="w-full h-full flex items-center justify-center">
                            <div className="text-center text-gray-300 px-4">
                              <Video className="size-12 mx-auto mb-3" />
                              <p className="font-medium">Waiting for live camera frames...</p>
                              <p className="text-sm text-gray-400 mt-1">
                                {isSocketConnected
                                  ? "Socket connected. Backend should start sending frames."
                                  : "Socket disconnected. Reconnect in progress..."}
                              </p>
                            </div>
                          </div>
                        )}
                        {/* Simulated timestamp overlay */}
                        <div className="absolute top-4 left-4 bg-black/60 text-white px-3 py-1 rounded text-sm font-mono">
                          {latestFrameAt ? new Date(latestFrameAt).toLocaleTimeString() : new Date().toLocaleTimeString()}
                        </div>
                        {/* Motion detection indicator */}
                        {motionDetection && (
                          <div className="absolute top-4 right-4 bg-red-500/80 text-white px-3 py-1 rounded text-sm flex items-center gap-2 animate-pulse">
                            <Bell className="size-4" />
                            Motion Detection ON
                          </div>
                        )}
                      </>
                    ) : (
                      <div className="w-full h-full flex items-center justify-center">
                        <div className="text-center text-gray-400">
                          <VideoOff className="size-16 mx-auto mb-4" />
                          <p>Stream Paused</p>
                        </div>
                      </div>
                    )}
                  </div>

                  {/* Controls */}
                  <div
                    className={`bg-gradient-to-r from-pink-100 to-purple-100 border-t border-purple-100 ${
                      isFullscreen ? "p-2 shrink-0" : "p-3"
                    }`}
                  >
                    <div className="flex items-center justify-between gap-3">
                      <div className="w-32 flex justify-start">
                        <Button
                          type="button"
                          variant="outline"
                          onClick={toggleStream}
                          size={isFullscreen ? "sm" : "default"}
                          className={controlButtonClass}
                        >
                          {isStreaming ? (
                            <>
                              <Video className="size-4 mr-2" />
                              Pause
                            </>
                          ) : (
                            <>
                              <VideoOff className="size-4 mr-2" />
                              Resume
                            </>
                          )}
                        </Button>
                      </div>

                      <div className="grid grid-cols-3 gap-1.5 w-[132px]">
                        <div />
                        <Button
                          type="button"
                          variant="outline"
                          size="sm"
                          onClick={() => handleCameraMove("up")}
                          aria-label="Move camera up"
                          className={controlButtonClass}
                        >
                          <ArrowUp className="size-4" />
                        </Button>
                        <div />
                        <Button
                          type="button"
                          variant="outline"
                          size="sm"
                          onClick={() => handleCameraMove("left")}
                          aria-label="Move camera left"
                          className={controlButtonClass}
                        >
                          <ArrowLeft className="size-4" />
                        </Button>
                        <div className="h-8" />
                        <Button
                          type="button"
                          variant="outline"
                          size="sm"
                          onClick={() => handleCameraMove("right")}
                          aria-label="Move camera right"
                          className={controlButtonClass}
                        >
                          <ArrowRight className="size-4" />
                        </Button>
                        <div />
                        <Button
                          type="button"
                          variant="outline"
                          size="sm"
                          onClick={() => handleCameraMove("down")}
                          aria-label="Move camera down"
                          className={controlButtonClass}
                        >
                          <ArrowDown className="size-4" />
                        </Button>
                        <div />
                      </div>

                      <div className="w-32 flex justify-end">
                        <Button
                          type="button"
                          onClick={handleFullscreen}
                          variant="outline"
                          size={isFullscreen ? "sm" : "default"}
                          className={controlButtonClass}
                        >
                          {isFullscreen ? <Minimize2 className="size-5" /> : <Maximize2 className="size-5" />}
                        </Button>
                      </div>
                    </div>
                  </div>
                </div>
              </CardContent>
            </Card>

            <Button
              type="button"
              onClick={handleBuzzerTrigger}
              disabled={isTriggeringBuzzer}
              className="w-full sm:w-auto border-0 bg-gradient-to-r from-pink-500 to-purple-500 text-white shadow-md transition-all hover:from-pink-600 hover:to-purple-600 hover:shadow-lg focus-visible:ring-pink-300 disabled:opacity-70"
            >
              <Bell className="size-4 mr-2" />
              {isTriggeringBuzzer ? "Sending buzzer signal..." : "Make a sound (distract cat)"}
            </Button>

          </div>

          {/* Sidebar */}
          <div className="space-y-4">
            {/* Activity Log */}
            <Card className="border-2 border-purple-200">
              <CardHeader className="bg-gradient-to-r from-pink-50 to-purple-50">
                <CardTitle className="flex items-center gap-2">
                  <PawPrint className="size-5 text-pink-500" />
                  Recent Activity
                </CardTitle>
              </CardHeader>
              <CardContent className="pt-4">
                {activity.length > 0 ? (
                  <div className="space-y-2">
                    {activity.map((act, idx) => (
                      <div
                        key={idx}
                        className="flex items-center gap-2 p-2 bg-purple-50 rounded text-sm"
                      >
                        {act}
                      </div>
                    ))}
                  </div>
                ) : (
                  <p className="text-gray-500 text-sm">No recent activity detected</p>
                )}
              </CardContent>
            </Card>

            {/* Settings */}
            <Card className="border-2 border-purple-200">
              <CardHeader className="bg-gradient-to-r from-pink-50 to-purple-50">
                <CardTitle className="flex items-center gap-2">
                  <Settings className="size-5 text-pink-500" />
                  Settings
                </CardTitle>
              </CardHeader>
              <CardContent className="pt-4 space-y-4">
                <div className="flex items-center justify-between">
                  <Label htmlFor="motion-detection" className="flex items-center gap-2 cursor-pointer">
                    <Bell className="size-4 text-purple-500" />
                    Notify me when motion is detected
                  </Label>
                  <Switch
                    id="motion-detection"
                    checked={motionDetection}
                    onCheckedChange={(checked) => {
                      setMotionDetection(checked);
                      toast.info(
                        checked ? "Motion alerts enabled 🔔" : "Motion alerts disabled 🔕"
                      );
                    }}
                  />
                </div>
              </CardContent>
            </Card>
          </div>
        </div>
      </div>
    </div>
  );
}
