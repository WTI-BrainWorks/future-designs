
function BWKbQueueDemo(index)
% To get the right index, use
%
%   idx = GetKeyboardIndices('Current Designs, Inc. 932');
%
% which *should* return the correct index. You could also peruse
% `PsychHID('Devices', 4)`.

ListenChar(-1);

canary = onCleanup(@() ListenChar(0));

KbName('UnifyKeyNames');
KbQueueCreate(index);
KbQueueStart(index);
canary2 = onCleanup(@() KbQueueRelease(index));

t0 = GetSecs();
t1 = t0 + 30;
press_arr = zeros(256, 1);
while GetSecs() < t1
   [~, firstpress, firstrelease] = KbQueueCheck(index);
   if any(firstpress)
       idx = find(firstpress, 1);
       name = KbName(idx);
       t = firstpress(idx) - t0;
       fprintf('Pressed %s at %.5f\n', name, t);
       press_arr(idx) = t;
   end

   if any(firstrelease)
       idx = find(firstrelease, 1);
       name = KbName(idx);
       t = firstrelease(idx) - t0;
       fprintf('Released %s at %.5f\n', name, t);
       v = press_arr(idx);
       if v > 0
           press_arr(idx) = 0;
           fprintf('Dur: %.5f\n', t - v);
       end
   end
end

end
